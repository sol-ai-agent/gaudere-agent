#!/bin/sh
set -eu

task_id=${1:-}
podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
agent_bin=${GAUDERE_AGENT_BIN:-}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
backup_directory=${GAUDERE_BACKUP_DIR:-"$data_home/gaudere/backups"}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
skip_service_check=${GAUDERE_SKIP_SERVICE_CHECK:-0}
expected_before=${GAUDERE_EXPECT_SCHEMA_BEFORE:-2}
expected_after=${GAUDERE_EXPECT_SCHEMA_AFTER:-3}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backup_script="$script_directory/backup-state.sh"

fail()
{
    printf 'gaudere schema deployment: %s\n' "$*" >&2
    exit 1
}

for command in tar sha256sum mktemp realpath cmp python3 date flock; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
[ -f "$backup_script" ] || fail "backup script not found: $backup_script"
[ -d "$state_directory" ] || fail "state directory not found: $state_directory"
[ -f "$state_directory/state.db" ] || fail "state database not found"

state_directory=$(realpath "$state_directory")
state_parent=$(dirname "$state_directory")
state_name=$(basename "$state_directory")
state_database="$state_directory/state.db"
mkdir -p "$backup_directory"
backup_directory=$(realpath "$backup_directory")

service_must_be_stopped()
{
    [ "$skip_service_check" = "1" ] && return 0
    command -v systemctl >/dev/null 2>&1 \
        || fail "systemctl is required for production service check"
    service_state=$(systemctl --user is-active "$service_name" 2>/dev/null || true)
    case "$service_state" in
        active|activating|reloading)
            fail "$service_name must be stopped before schema deployment"
            ;;
    esac
}

state_lock_must_be_free()
{
    exec 9>>"$state_database.lock"
    chmod 600 "$state_database.lock" 2>/dev/null || true
    if ! flock -n 9; then
        fail "state database is currently owned"
    fi
    flock -u 9
}

schema_version()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    print(db.execute("PRAGMA user_version").fetchone()[0])
PY
}

logical_snapshot()
{
    database=$1
    output=$2
    python3 - "$database" > "$output" <<'PY'
import json
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as db:
    def exists(name):
        return db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
        ).fetchone() is not None

    result = {}
    if exists("tasks"):
        columns = (
            "id,idempotency_key,kind,input_content_type,input,"
            "max_input_bytes,max_output_bytes,max_runtime_ms,max_attempts,"
            "attempts_started,status,lease_owner,lease_expires_at_ms,cancel_reason,"
            "result_content_type,result_output,result_failure_code,result_failure_message"
        )
        result["tasks"] = db.execute(
            f"SELECT {columns} FROM tasks ORDER BY id"
        ).fetchall()
    else:
        result["tasks"] = None

    if exists("actions"):
        result["actions"] = db.execute(
            "SELECT id,idempotency_key,critical,status,effect_result,"
            "lease_owner,lease_expires_at_ms FROM actions ORDER BY id"
        ).fetchall()
    else:
        result["actions"] = None

    if exists("budget_consumptions"):
        result["budget_consumptions"] = db.execute(
            "SELECT scope,idempotency_key,consumed_at_ms "
            "FROM budget_consumptions ORDER BY scope,idempotency_key"
        ).fetchall()
    else:
        result["budget_consumptions"] = None

    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
PY
}

run_agent_on()
{
    directory=$1
    shift
    if [ -n "$agent_bin" ]; then
        "$agent_bin" --state "$directory/state.db" "$@"
        return
    fi

    command -v "$podman_command" >/dev/null 2>&1 \
        || fail "required command not found: $podman_command"
    "$podman_command" image exists "$image" \
        || fail "image does not exist: $image"

    "$podman_command" run --rm \
        --network none \
        --userns keep-id \
        --read-only \
        --read-only-tmpfs=true \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$directory:/var/lib/gaudere:Z" \
        "$image" \
        --state /var/lib/gaudere/state.db "$@"
}

service_must_be_stopped
state_lock_must_be_free

before_version=$(schema_version "$state_database")
printf 'schema_before=%s\n' "$before_version"
[ "$before_version" = "$expected_before" ] \
    || fail "expected production schema $expected_before, found $before_version"

printf '\n==> create fresh stopped-state backup\n'
archive=$(GAUDERE_STATE_DIR="$state_directory" \
    GAUDERE_BACKUP_DIR="$backup_directory" sh "$backup_script")
[ -f "$archive" ] || fail "fresh backup was not created"
[ -f "$archive.sha256" ] || fail "fresh backup checksum was not created"
(
    cd "$backup_directory"
    sha256sum -c "$(basename "$archive.sha256")"
) || fail "fresh backup checksum verification failed"
printf 'backup=%s\n' "$archive"

# The service must still be stopped after backup creation. This catches accidental
# manual/systemd restarts before any state path is replaced.
service_must_be_stopped
state_lock_must_be_free

stamp=$(date -u +%Y%m%dT%H%M%SZ)
staging=$(mktemp -d "$state_parent/.${state_name}.v3-staging.XXXXXX")
rollback="$state_parent/${state_name}.pre-v3-$stamp"
failed_state="$state_parent/${state_name}.failed-v3-$stamp"
[ ! -e "$rollback" ] || fail "rollback path already exists: $rollback"
[ ! -e "$failed_state" ] || fail "failed-state path already exists: $failed_state"

swap_started=0
swap_complete=0
cleanup()
{
    if [ "$swap_started" = "1" ] && [ "$swap_complete" != "1" ]; then
        if [ ! -e "$state_directory" ] && [ -d "$rollback" ]; then
            mv "$rollback" "$state_directory" 2>/dev/null || true
        fi
    fi
    if [ -d "$staging" ]; then
        rm -rf "$staging"
    fi
}
trap cleanup EXIT HUP INT TERM

printf '\n==> restore fresh backup into staging state\n'
tar -xzf "$archive" -C "$staging"
[ -f "$staging/state.db" ] || fail "staged backup has no state.db"
[ ! -e "$staging/state.db.lock" ] \
    || fail "staged backup unexpectedly contains state.db.lock"

staged_before=$(schema_version "$staging/state.db")
printf 'staged_schema_before=%s\n' "$staged_before"
[ "$staged_before" = "$expected_before" ] \
    || fail "fresh backup restored with unexpected schema $staged_before"
logical_snapshot "$staging/state.db" "$staging/logical.before.json"

printf '\n==> migrate and validate staging state with networking disabled\n'
check_output=$(run_agent_on "$staging" --check)
printf '%s\n' "$check_output"
printf '%s\n' "$check_output" | grep -q '^gaudere-agent: running$' \
    || fail "staging migration did not reach running readiness"
printf '%s\n' "$check_output" | grep -q '^gaudere-agent: safe$' \
    || fail "staging migration did not finish safe"

staged_after=$(schema_version "$staging/state.db")
printf 'staged_schema_after=%s\n' "$staged_after"
[ "$staged_after" = "$expected_after" ] \
    || fail "staging migration expected schema $expected_after, found $staged_after"
logical_snapshot "$staging/state.db" "$staging/logical.after.json"
cmp -s "$staging/logical.before.json" "$staging/logical.after.json" \
    || fail "logical durable rows changed during staging migration"

metadata_rows=$(python3 - "$staging/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    print(db.execute(
        "SELECT COUNT(*) FROM tasks "
        "WHERE result_metadata_content_type IS NOT NULL OR result_metadata IS NOT NULL"
    ).fetchone()[0])
PY
)
[ "$metadata_rows" = "0" ] \
    || fail "historical tasks unexpectedly gained result metadata"

budget_rows=$(python3 - "$staging/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    print(db.execute("SELECT COUNT(*) FROM budget_consumptions").fetchone()[0])
PY
)
printf 'budget_consumptions=%s\n' "$budget_rows"

if [ -n "$task_id" ]; then
    printf '\n==> inspect representative task in staging state\n'
    run_agent_on "$staging" --task "$task_id"
fi

# Final guard immediately before the path swap.
service_must_be_stopped
state_lock_must_be_free
current_version=$(schema_version "$state_database")
[ "$current_version" = "$expected_before" ] \
    || fail "production state changed before swap (schema=$current_version)"

printf '\n==> atomically stage rollback and install migrated state\n'
mv "$state_directory" "$rollback"
swap_started=1
if ! mv "$staging" "$state_directory"; then
    mv "$rollback" "$state_directory" 2>/dev/null || true
    swap_started=0
    fail "cannot install migrated state; original state restored"
fi
swap_complete=1

installed_version=$(schema_version "$state_directory/state.db")
printf 'schema_after=%s\n' "$installed_version"
[ "$installed_version" = "$expected_after" ] \
    || fail "installed state does not report schema $expected_after"

if [ -n "$task_id" ]; then
    printf '\n==> inspect representative task in installed state\n'
    run_agent_on "$state_directory" --task "$task_id"
fi

trap - EXIT HUP INT TERM

printf '\n==> staged production schema deployment complete\n'
printf 'backup=%s\n' "$archive"
printf 'rollback_directory=%s\n' "$rollback"
printf 'failed_state_directory_if_needed=%s\n' "$failed_state"
printf 'gaudere staged production schema deployment: READY\n'
printf 'Service remains stopped; start and validate it explicitly before removing rollback material.\n'
