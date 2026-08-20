#!/bin/sh
set -eu

archive=${1:-}
task_id=${2:-}
podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
agent_bin=${GAUDERE_AGENT_BIN:-}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
expected_before=${GAUDERE_EXPECT_SCHEMA_BEFORE:-2}
expected_after=${GAUDERE_EXPECT_SCHEMA_AFTER:-3}

fail()
{
    printf 'gaudere schema migration validation: %s\n' "$*" >&2
    exit 1
}

[ -n "$archive" ] || fail "usage: $0 BACKUP_ARCHIVE [TASK_ID]"
[ -f "$archive" ] || fail "backup archive not found: $archive"

for command in tar sha256sum mktemp realpath cmp python3; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done

archive=$(realpath "$archive")
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/schema-migration.XXXXXX")
state="$workspace/state"
mkdir -p "$state"

cleanup()
{
    if [ "${KEEP_GAUDERE_VALIDATION_STATE:-0}" = "1" ]; then
        printf 'schema migration validation state kept at %s\n' "$workspace" >&2
    else
        rm -rf "$workspace"
    fi
}
trap cleanup EXIT HUP INT TERM

sha256sum "$archive" > "$workspace/archive.before.sha256"
if [ -f "$archive.sha256" ]; then
    (
        cd "$(dirname "$archive")"
        sha256sum -c "$(basename "$archive.sha256")" >/dev/null
    ) || fail "backup checksum verification failed"
fi

printf '\n==> restore backup into disposable state\n'
tar -xzf "$archive" -C "$state"
[ -f "$state/state.db" ] || fail "restored backup has no state.db"
[ ! -e "$state/state.db.lock" ] \
    || fail "backup unexpectedly contains coordination-only state.db.lock"

state_db="$state/state.db"

schema_version()
{
    python3 - "$state_db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    print(db.execute("PRAGMA user_version").fetchone()[0])
PY
}

logical_snapshot()
{
    output=$1
    python3 - "$state_db" > "$output" <<'PY'
import json
import sqlite3
import sys

path = sys.argv[1]
with sqlite3.connect(path) as db:
    def exists(name):
        return db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
        ).fetchone() is not None

    result = {}
    if exists("tasks"):
        task_columns = (
            "id,idempotency_key,kind,input_content_type,input,"
            "max_input_bytes,max_output_bytes,max_runtime_ms,max_attempts,"
            "attempts_started,status,lease_owner,lease_expires_at_ms,cancel_reason,"
            "result_content_type,result_output,result_failure_code,result_failure_message"
        )
        result["tasks"] = db.execute(
            f"SELECT {task_columns} FROM tasks ORDER BY id"
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

run_agent()
{
    if [ -n "$agent_bin" ]; then
        "$agent_bin" --state "$state_db" "$@"
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
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$state:/var/lib/gaudere:Z" \
        "$image" \
        --state /var/lib/gaudere/state.db "$@"
}

before_version=$(schema_version)
printf 'schema_before=%s\n' "$before_version"
[ "$before_version" = "$expected_before" ] \
    || fail "expected source schema $expected_before, found $before_version"
logical_snapshot "$workspace/logical.before.json"

printf '\n==> migrate only the disposable copy with networking disabled\n'
check_output=$(run_agent --check)
printf '%s\n' "$check_output"
printf '%s\n' "$check_output" | grep -q '^gaudere-agent: running$' \
    || fail "migration check did not reach running readiness"
printf '%s\n' "$check_output" | grep -q '^gaudere-agent: safe$' \
    || fail "migration check did not finish safe"

after_version=$(schema_version)
printf 'schema_after=%s\n' "$after_version"
[ "$after_version" = "$expected_after" ] \
    || fail "expected migrated schema $expected_after, found $after_version"
logical_snapshot "$workspace/logical.after.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.after.json" \
    || fail "durable task/action/budget rows changed during schema migration"

metadata_rows=$(python3 - "$state_db" <<'PY'
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
    || fail "legacy rows unexpectedly gained result metadata during migration"

if [ -n "$task_id" ]; then
    printf '\n==> inspect representative durable task after migration\n'
    run_agent --task "$task_id"
fi

printf '\n==> prove source backup archive was not modified\n'
sha256sum "$archive" > "$workspace/archive.after.sha256"
cmp -s "$workspace/archive.before.sha256" "$workspace/archive.after.sha256" \
    || fail "source backup archive changed during validation"

printf '\n==> schema migration copy validation complete\n'
printf 'gaudere schema migration copy validation: PASS\n'
