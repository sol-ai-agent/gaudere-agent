#!/bin/sh
set -eu

# PREP ONLY / NOT AUTHORIZED FOR PRODUCTION.
#
# This script prepares the reversible, stopped-state v3 -> v4 directory swap.
# It deliberately does not start, stop, install, or reconfigure a service. The
# caller must provide every live path explicitly, and the service must already be
# inactive. Candidate/rollback image provenance is a separate gate.

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
agent_bin=${GAUDERE_AGENT_BIN:-}
test_mode=${GAUDERE_TEST_MODE:-0}
state_directory=${GAUDERE_STATE_DIR:-}
backup_directory=${GAUDERE_BACKUP_DIR:-}
service_name=${GAUDERE_SERVICE_NAME:-}
image=${GAUDERE_IMAGE:-}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backup_script="$script_directory/backup-state.sh"
phase=preflight
workspace=
staging=
rollback=
failed_state=
swap_started=0
swap_complete=0

fail()
{
    printf 'gaudere schema v4 deployment: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

[ "$#" -eq 0 ] \
    || fail "usage: GAUDERE_STATE_DIR=... GAUDERE_BACKUP_DIR=... GAUDERE_SERVICE_NAME=... $0"

case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac

[ -n "$state_directory" ] \
    || fail "GAUDERE_STATE_DIR must be set explicitly"
[ -n "$backup_directory" ] \
    || fail "GAUDERE_BACKUP_DIR must be set explicitly"
[ -n "$service_name" ] \
    || fail "GAUDERE_SERVICE_NAME must be set explicitly"

if [ -n "$agent_bin" ]; then
    [ "$test_mode" = "1" ] \
        || fail "GAUDERE_AGENT_BIN is restricted to synthetic test mode"
    [ -x "$agent_bin" ] \
        || fail "Agent binary is not executable: $agent_bin"
else
    [ -n "$image" ] \
        || fail "GAUDERE_IMAGE must name an explicitly selected candidate image"
fi

for command in basename cat chmod cmp date dirname flock grep mkdir mktemp mv \
        python3 realpath rm sha256sum tar; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
command -v "$systemctl_command" >/dev/null 2>&1 \
    || fail "required command not found: $systemctl_command"
[ -f "$backup_script" ] || fail "backup script not found: $backup_script"
[ -d "$state_directory" ] \
    || fail "state directory not found: $state_directory"
[ ! -L "$state_directory" ] \
    || fail "state directory must not be a symbolic link"
[ -f "$state_directory/state.db" ] \
    || fail "state database not found: $state_directory/state.db"
[ ! -L "$state_directory/state.db" ] \
    || fail "state database must not be a symbolic link"
[ ! -e "$backup_directory" ] || [ ! -L "$backup_directory" ] \
    || fail "backup directory must not be a symbolic link"

state_directory=$(realpath "$state_directory")
state_parent=$(dirname "$state_directory")
state_name=$(basename "$state_directory")
state_database="$state_directory/state.db"
backup_directory=$(realpath -m "$backup_directory")
case "$backup_directory/" in
    "$state_directory/"*)
        fail "backup directory must not be inside the state directory"
        ;;
esac

observed_service_state=
service_is_inactive()
{
    observed_service_state=$(
        "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
    )
    [ "$observed_service_state" = "inactive" ]
}

service_must_be_inactive()
{
    service_is_inactive || fail \
        "$service_name must report exactly inactive (found ${observed_service_state:-unknown})"
}

state_lock_must_be_free()
{
    exec 9>>"$state_database.lock"
    chmod 600 "$state_database.lock" 2>/dev/null || true
    if ! flock -n 9; then
        fail "state database is currently owned"
    fi
    flock -u 9
    exec 9>&-
}

hold_source_lock()
{
    exec 9>>"$state_database.lock"
    chmod 600 "$state_database.lock" 2>/dev/null || true
    flock -n 9 || fail "state database became owned after backup creation"
}

schema_version()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    print(db.execute("PRAGMA user_version").fetchone()[0])
PY
}

verify_quiescent_v3()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    version = db.execute("PRAGMA user_version").fetchone()[0]
    if version != 3:
        raise SystemExit(f"expected schema 3, found {version}")
    if [row[0] for row in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("SQLite integrity_check failed")

    tables = {row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table'"
    )}
    for required in ("tasks", "actions", "budget_consumptions"):
        if required not in tables:
            raise SystemExit(f"required durable table is missing: {required}")

    wake_objects = db.execute(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE name='wake_intents' OR tbl_name='wake_intents'"
    ).fetchone()[0]
    if wake_objects != 0:
        raise SystemExit("schema v3 unexpectedly contains wake-intent objects")

    task = db.execute(
        "SELECT id,status FROM tasks WHERE status IN (0,1,2) "
        "ORDER BY rowid LIMIT 1"
    ).fetchone()
    if task:
        raise SystemExit(f"nonterminal Task blocks migration: {task[0]}:{task[1]}")
    action = db.execute(
        "SELECT id,status FROM actions WHERE status IN (0,1,2) "
        "ORDER BY rowid LIMIT 1"
    ).fetchone()
    if action:
        raise SystemExit(f"nonterminal Action blocks migration: {action[0]}:{action[1]}")

    leased_task = db.execute(
        "SELECT id FROM tasks WHERE lease_owner IS NOT NULL "
        "OR lease_expires_at_ms IS NOT NULL ORDER BY rowid LIMIT 1"
    ).fetchone()
    if leased_task:
        raise SystemExit(f"Task lease remains in stopped state: {leased_task[0]}")
    leased_action = db.execute(
        "SELECT id FROM actions WHERE lease_owner IS NOT NULL "
        "OR lease_expires_at_ms IS NOT NULL ORDER BY rowid LIMIT 1"
    ).fetchone()
    if leased_action:
        raise SystemExit(f"Action lease remains in stopped state: {leased_action[0]}")
PY
}

logical_snapshot()
{
    database=$1
    output=$2
    python3 - "$database" > "$output" <<'PY'
import base64
import json
import sqlite3
import sys
import urllib.parse

def encode(value):
    if isinstance(value, bytes):
        return {"bytes_base64": base64.b64encode(value).decode("ascii")}
    return value

def quote_identifier(value):
    return '"' + value.replace('"', '""') + '"'

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    if [row[0] for row in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("SQLite integrity_check failed")
    objects = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' AND tbl_name!='wake_intents' "
        "ORDER BY type,name"
    ).fetchall()
    tables = [row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' AND name!='wake_intents' ORDER BY name"
    )]
    contents = {}
    for table in tables:
        quoted = quote_identifier(table)
        columns = db.execute(f"PRAGMA table_xinfo({quoted})").fetchall()
        rows = [[encode(value) for value in row]
                for row in db.execute(f"SELECT * FROM {quoted}").fetchall()]
        rows.sort(key=lambda row: json.dumps(
            row, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
        contents[table] = {"columns": columns, "rows": rows}
    result = {
        "application_id": db.execute("PRAGMA application_id").fetchone()[0],
        "encoding": db.execute("PRAGMA encoding").fetchone()[0],
        "objects": objects,
        "tables": contents,
    }
print(json.dumps(result, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
PY
}

budget_snapshot()
{
    database=$1
    output=$2
    python3 - "$database" > "$output" <<'PY'
import json
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    objects = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE tbl_name='budget_consumptions' ORDER BY type,name"
    ).fetchall()
    columns = db.execute("PRAGMA table_xinfo(budget_consumptions)").fetchall()
    rows = db.execute(
        "SELECT * FROM budget_consumptions ORDER BY scope,idempotency_key"
    ).fetchall()
print(json.dumps(
    {"objects": objects, "columns": columns, "rows": rows},
    ensure_ascii=False, sort_keys=True, separators=(",", ":")))
PY
}

provider_budget_rows()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    print(db.execute(
        "SELECT COUNT(*) FROM budget_consumptions WHERE scope=?",
        ("provider.call:openai.responses",),
    ).fetchone()[0])
PY
}

verify_empty_wake_schema()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    if db.execute("PRAGMA user_version").fetchone()[0] != 4:
        raise SystemExit("wake schema validation requires schema 4")
    if [row[0] for row in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("SQLite integrity_check failed")
    columns = [row[1] for row in db.execute("PRAGMA table_xinfo(wake_intents)")]
    expected_columns = [
        "scope", "id", "source_id", "accepted_at_ms", "due_at_ms", "status",
        "terminal_at_ms", "terminal_reason",
    ]
    if columns != expected_columns:
        raise SystemExit("wake_intents columns do not match schema v4")
    objects = set(db.execute(
        "SELECT type,name FROM sqlite_master WHERE tbl_name='wake_intents' "
        "AND name NOT LIKE 'sqlite_autoindex_%'"
    ).fetchall())
    expected_objects = {
        ("table", "wake_intents"),
        ("index", "idx_wake_intents_scope_status_due"),
        ("trigger", "wake_intents_require_scheduled_insert"),
        ("trigger", "wake_intents_single_transition"),
        ("trigger", "wake_intents_prevent_delete"),
    }
    if objects != expected_objects:
        raise SystemExit("wake_intents objects do not match schema v4")
    counts = {
        "scheduled": db.execute(
            "SELECT COUNT(*) FROM wake_intents WHERE status=0").fetchone()[0],
        "fired": db.execute(
            "SELECT COUNT(*) FROM wake_intents WHERE status=1").fetchone()[0],
        "revoked": db.execute(
            "SELECT COUNT(*) FROM wake_intents WHERE status=2").fetchone()[0],
        "manual_review": db.execute(
            "SELECT COUNT(*) FROM wake_intents WHERE status=3").fetchone()[0],
    }
    total = db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0]
    if total != 0 or any(counts.values()):
        raise SystemExit("schema v4 contains a wake intent")
    print(f"wake_rows={total}")
    print(f"wake_scheduled={counts['scheduled']}")
    print(f"wake_armed={counts['scheduled']}")
    print(f"wake_fired={counts['fired']}")
    print(f"wake_revoked={counts['revoked']}")
    print(f"wake_manual_review={counts['manual_review']}")
PY
}

verify_archive()
{
    archive=$1
    checksum=$2
    python3 - "$archive" "$checksum" <<'PY'
import hashlib
import pathlib
import re
import sys
import tarfile

archive = pathlib.Path(sys.argv[1])
checksum = pathlib.Path(sys.argv[2])
if archive.is_symlink() or checksum.is_symlink():
    raise SystemExit("backup archive and checksum must not be symbolic links")
lines = checksum.read_text(encoding="ascii").splitlines()
if len(lines) != 1:
    raise SystemExit("checksum manifest must contain exactly one entry")
match = re.fullmatch(r"([0-9a-f]{64})  ([^/]+)", lines[0])
if match is None or match.group(2) != archive.name:
    raise SystemExit("checksum manifest does not name the backup archive exactly")
digest = hashlib.sha256()
with archive.open("rb") as source:
    for block in iter(lambda: source.read(1024 * 1024), b""):
        digest.update(block)
if digest.hexdigest() != match.group(1):
    raise SystemExit("backup archive checksum mismatch")

has_state = False
seen = set()
with tarfile.open(archive, "r:gz") as source:
    for member in source.getmembers():
        path = pathlib.PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts:
            raise SystemExit("backup contains an unsafe path")
        normalized = path.as_posix()
        if normalized in seen:
            raise SystemExit("backup contains a duplicate member path")
        seen.add(normalized)
        if not (member.isdir() or member.isfile()):
            raise SystemExit("backup contains a link or unsupported file type")
        if path.name == "state.db.lock":
            raise SystemExit("backup contains coordination-only state.db.lock")
        if member.isfile() and path == pathlib.PurePosixPath("state.db"):
            has_state = True
if not has_state:
    raise SystemExit("backup has no top-level state.db")
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
        || fail "candidate image does not exist: $image"
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

archive_is_unchanged()
{
    sha256sum "$archive" > "$workspace/archive.current.sha256"
    sha256sum "$archive.sha256" > "$workspace/manifest.current.sha256"
    cmp -s "$workspace/archive.before.sha256" \
        "$workspace/archive.current.sha256" \
        && cmp -s "$workspace/manifest.before.sha256" \
            "$workspace/manifest.current.sha256"
}

cleanup()
{
    exit_status=$?
    trap - EXIT HUP INT TERM
    rollback_ok=1

    if [ "$swap_started" = "1" ] && [ "$swap_complete" != "1" ]; then
        printf 'automatic_rollback=STARTED\n' >&2
        if ! service_is_inactive; then
            printf 'gaudere schema v4 deployment: rollback blocked: %s is %s\n' \
                "$service_name" "${observed_service_state:-unknown}" >&2
            rollback_ok=0
        fi

        flock -u 8 2>/dev/null || true
        exec 8>&-

        if [ "$rollback_ok" = "1" ] && [ -e "$state_directory" ]; then
            if [ ! -d "$state_directory" ]; then
                printf '%s\n' \
                    'gaudere schema v4 deployment: rollback blocked: installed state path is not a directory' >&2
                rollback_ok=0
            elif [ -f "$state_directory/state.db" ]; then
                if ! exec 7>>"$state_directory/state.db.lock"; then
                    printf '%s\n' \
                        'gaudere schema v4 deployment: rollback blocked: cannot open installed state lock' >&2
                    rollback_ok=0
                elif ! flock -n 7; then
                    printf '%s\n' \
                        'gaudere schema v4 deployment: rollback blocked: installed state is owned' >&2
                    rollback_ok=0
                fi
            fi
        fi

        if [ "$rollback_ok" = "1" ] && [ -e "$state_directory" ]; then
            if [ -e "$failed_state" ]; then
                printf 'gaudere schema v4 deployment: rollback blocked: failed-state path appeared: %s\n' \
                    "$failed_state" >&2
                rollback_ok=0
            elif ! mv "$state_directory" "$failed_state"; then
                printf '%s\n' \
                    'gaudere schema v4 deployment: rollback could not retain failed v4 state' >&2
                rollback_ok=0
            fi
        fi

        if [ "$rollback_ok" = "1" ]; then
            if [ ! -d "$rollback" ] || [ -e "$state_directory" ]; then
                printf 'gaudere schema v4 deployment: rollback layout is ambiguous\n' >&2
                rollback_ok=0
            elif ! mv "$rollback" "$state_directory"; then
                printf '%s\n' \
                    'gaudere schema v4 deployment: rollback could not restore original v3 state' >&2
                rollback_ok=0
            fi
        fi

        if [ "$rollback_ok" = "1" ]; then
            if ! verify_quiescent_v3 "$state_directory/state.db" \
                || ! logical_snapshot "$state_directory/state.db" \
                    "$workspace/logical.rollback-restored.json" \
                || ! cmp -s "$workspace/logical.before.json" \
                    "$workspace/logical.rollback-restored.json" \
                || ! budget_snapshot "$state_directory/state.db" \
                    "$workspace/budget.rollback-restored.json" \
                || ! cmp -s "$workspace/budget.before.json" \
                    "$workspace/budget.rollback-restored.json"; then
                printf '%s\n' \
                    'gaudere schema v4 deployment: restored v3 state failed rollback validation' >&2
                rollback_ok=0
            fi
        fi

        if [ "$rollback_ok" = "1" ]; then
            printf 'automatic_rollback=PASS\n' >&2
            printf 'failed_v4_directory=%s\n' "$failed_state" >&2
        else
            printf 'automatic_rollback=MANUAL_REVIEW\n' >&2
            printf 'observed_state_directory=%s\n' "$state_directory" >&2
            printf 'observed_rollback_directory=%s\n' "$rollback" >&2
            printf 'observed_failed_v4_directory=%s\n' "$failed_state" >&2
        fi
    fi

    flock -u 7 2>/dev/null || true
    exec 7>&-
    flock -u 9 2>/dev/null || true
    exec 9>&-

    if [ -n "$workspace" ] && [ -d "$workspace" ] \
        && { [ "$swap_started" != "1" ] || [ "$rollback_ok" = "1" ]; }; then
        rm -rf "$workspace" || true
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'retained_workspace=%s\n' "$workspace" >&2
    fi
    exit "$exit_status"
}

service_must_be_inactive
state_lock_must_be_free

stamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
workspace=$(mktemp -d "$state_parent/.${state_name}.v4-work.XXXXXX")
staging="$workspace/state"
rollback_verify="$workspace/rollback-verify"
rollback="$state_parent/${state_name}.pre-v4-$stamp"
failed_state="$state_parent/${state_name}.failed-v4-$stamp"
[ ! -e "$rollback" ] || fail "rollback path already exists: $rollback"
[ ! -e "$failed_state" ] || fail "failed-state path already exists: $failed_state"
mkdir -p "$staging" "$rollback_verify"
trap cleanup EXIT HUP INT TERM

printf 'status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION\n'
printf 'state_directory=%s\n' "$state_directory"
printf 'backup_directory=%s\n' "$backup_directory"
printf 'staging_workspace=%s\n' "$workspace"
printf 'planned_rollback_directory=%s\n' "$rollback"
printf 'service_state=inactive\n'

phase=fresh-backup
printf '\n==> create and verify a fresh stopped-state backup\n'
if ! archive=$(GAUDERE_STATE_DIR="$state_directory" \
        GAUDERE_BACKUP_DIR="$backup_directory" sh "$backup_script"); then
    fail "fresh backup creation failed"
fi
[ -f "$archive" ] || fail "fresh backup was not created"
[ -f "$archive.sha256" ] || fail "fresh backup checksum was not created"
archive=$(realpath "$archive")
[ "$(realpath "$archive.sha256")" = "$archive.sha256" ] \
    || fail "backup checksum path is not canonical"
verify_archive "$archive" "$archive.sha256" \
    || fail "fresh backup verification failed"
sha256sum "$archive" > "$workspace/archive.before.sha256"
sha256sum "$archive.sha256" > "$workspace/manifest.before.sha256"
printf 'backup=%s\n' "$archive"
printf 'backup_verified=PASS\n'

phase=source-fence
service_must_be_inactive
hold_source_lock
[ ! -e "$state_database-wal" ] \
    || fail "unexpected SQLite WAL remains in stopped state"
[ ! -e "$state_database-shm" ] \
    || fail "unexpected SQLite shared-memory file remains in stopped state"
verify_quiescent_v3 "$state_database" \
    || fail "source v3 state is not quiescent"
before_version=$(schema_version "$state_database")
printf 'schema_before=%s\n' "$before_version"
logical_snapshot "$state_database" "$workspace/logical.before.json"
budget_snapshot "$state_database" "$workspace/budget.before.json"
budget_before=$(provider_budget_rows "$state_database")
printf 'provider_budget_rows_before=%s\n' "$budget_before"

phase=restore
printf '\n==> restore the verified backup into disposable staging and rollback copies\n'
tar --no-same-owner --no-same-permissions \
    -xzf "$archive" -C "$staging"
tar --no-same-owner --no-same-permissions \
    -xzf "$archive" -C "$rollback_verify"
for restored in "$staging" "$rollback_verify"; do
    [ -f "$restored/state.db" ] \
        || fail "restored backup has no state.db: $restored"
    [ ! -e "$restored/state.db.lock" ] \
        || fail "restored backup unexpectedly contains state.db.lock"
    verify_quiescent_v3 "$restored/state.db" \
        || fail "restored backup is not a quiescent schema-v3 state"
done
logical_snapshot "$staging/state.db" "$workspace/logical.restored.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.restored.json" \
    || fail "fresh backup differs from the locked source state"
logical_snapshot "$rollback_verify/state.db" \
    "$workspace/logical.rollback-verify.json"
cmp -s "$workspace/logical.before.json" \
    "$workspace/logical.rollback-verify.json" \
    || fail "independent rollback restore differs from the source state"
budget_snapshot "$staging/state.db" "$workspace/budget.restored.json"
cmp -s "$workspace/budget.before.json" "$workspace/budget.restored.json" \
    || fail "provider budget differs in the restored backup"
printf 'staged_schema_before=%s\n' "$(schema_version "$staging/state.db")"
printf 'rollback_restore_schema=3\n'

phase=staging-migration
printf '\n==> migrate only the disposable copy with wake schema explicitly enabled\n'
if ! migration_output=$(run_agent_on "$staging" --check --wake-intents 2>&1); then
    printf '%s\n' "$migration_output" >&2
    fail "staging migration failed"
fi
printf '%s\n' "$migration_output"
printf '%s\n' "$migration_output" | grep -q \
    '^gaudere-agent: explicit wake enabled scope=cognition.reflect.wake.v0 max_total=1 automatic_successor=false$' \
    || fail "staging migration did not explicitly enable the fixed wake schema"
printf '%s\n' "$migration_output" | grep -q '^gaudere-agent: running$' \
    || fail "staging migration did not reach readiness"
printf '%s\n' "$migration_output" | grep -q '^gaudere-agent: safe$' \
    || fail "staging migration did not finish safe"

staged_after=$(schema_version "$staging/state.db")
printf 'staged_schema_after=%s\n' "$staged_after"
[ "$staged_after" = "4" ] || fail "staging migration did not produce schema 4"
verify_empty_wake_schema "$staging/state.db" > "$workspace/wake.staged.out" \
    || fail "staging wake schema validation failed"
cat "$workspace/wake.staged.out"
logical_snapshot "$staging/state.db" "$workspace/logical.staged.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.staged.json" \
    || fail "Tasks, Actions, Budgets, or provider metadata changed during migration"
budget_snapshot "$staging/state.db" "$workspace/budget.staged.json"
cmp -s "$workspace/budget.before.json" "$workspace/budget.staged.json" \
    || fail "provider budget changed during migration"
budget_staged=$(provider_budget_rows "$staging/state.db")
printf 'provider_budget_rows_staged=%s\n' "$budget_staged"
[ "$budget_staged" = "$budget_before" ] \
    || fail "provider budget row count changed during migration"

phase=staging-reopen
if ! opt_in_reopen=$(run_agent_on "$staging" --check --wake-intents 2>&1); then
    printf '%s\n' "$opt_in_reopen" >&2
    fail "schema-v4 opt-in reopen failed"
fi
printf '%s\n' "$opt_in_reopen" | grep -q '^gaudere-agent: safe$' \
    || fail "schema-v4 opt-in reopen did not finish safe"
if ! default_reopen=$(run_agent_on "$staging" --check 2>&1); then
    printf '%s\n' "$default_reopen" >&2
    fail "schema-v4 default reopen failed"
fi
printf '%s\n' "$default_reopen" | grep -q '^gaudere-agent: safe$' \
    || fail "schema-v4 default reopen did not finish safe"
if printf '%s\n' "$default_reopen" | grep -q 'explicit wake enabled'; then
    fail "default schema-v4 reopen unexpectedly enabled wake intents"
fi
verify_empty_wake_schema "$staging/state.db" >/dev/null \
    || fail "schema-v4 reopen changed wake state"
logical_snapshot "$staging/state.db" "$workspace/logical.reopened.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.reopened.json" \
    || fail "schema-v4 reopen changed durable non-wake state"
budget_snapshot "$staging/state.db" "$workspace/budget.reopened.json"
cmp -s "$workspace/budget.before.json" "$workspace/budget.reopened.json" \
    || fail "schema-v4 reopen changed provider budget"
archive_is_unchanged || fail "backup or checksum changed during staging validation"
printf 'staging_reopen=PASS\n'

phase=pre-swap-fence
printf '\n==> repeat the stopped-state fence before the directory swap\n'
service_must_be_inactive
verify_quiescent_v3 "$state_database" \
    || fail "source state drifted before swap"
logical_snapshot "$state_database" "$workspace/logical.current.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.current.json" \
    || fail "source durable state changed after backup creation"
budget_snapshot "$state_database" "$workspace/budget.current.json"
cmp -s "$workspace/budget.before.json" "$workspace/budget.current.json" \
    || fail "source provider budget changed after backup creation"
[ ! -e "$rollback" ] || fail "rollback path appeared before swap"
[ ! -e "$failed_state" ] || fail "failed-state path appeared before swap"
exec 8>>"$staging/state.db.lock"
chmod 600 "$staging/state.db.lock" 2>/dev/null || true
flock -n 8 || fail "staging state became owned before swap"

phase=swap-source
printf '\n==> retain v3 rollback tree and install the proven v4 staging tree\n'
swap_started=1
if ! mv "$state_directory" "$rollback"; then
    swap_started=0
    fail "cannot retain the original v3 state"
fi
phase=swap-install
if ! mv "$staging" "$state_directory"; then
    fail "cannot install the staged v4 state"
fi
flock -u 8
exec 8>&-

phase=post-swap-default-reopen
service_must_be_inactive
if ! installed_output=$(run_agent_on "$state_directory" --check 2>&1); then
    printf '%s\n' "$installed_output" >&2
    fail "installed schema-v4 default reopen failed"
fi
printf '%s\n' "$installed_output"
printf '%s\n' "$installed_output" | grep -q '^gaudere-agent: safe$' \
    || fail "installed schema-v4 check did not finish safe"
if printf '%s\n' "$installed_output" | grep -q 'explicit wake enabled'; then
    fail "installed default profile unexpectedly enabled wake intents"
fi

phase=post-swap-audit
service_must_be_inactive
exec 8>>"$state_directory/state.db.lock"
chmod 600 "$state_directory/state.db.lock" 2>/dev/null || true
flock -n 8 || fail "installed schema-v4 state is owned during audit"
installed_version=$(schema_version "$state_directory/state.db")
printf 'schema_after=%s\n' "$installed_version"
[ "$installed_version" = "4" ] \
    || fail "installed state does not report schema 4"
verify_empty_wake_schema "$state_directory/state.db" \
    > "$workspace/wake.installed.out" \
    || fail "installed wake schema validation failed"
cat "$workspace/wake.installed.out"
logical_snapshot "$state_directory/state.db" "$workspace/logical.installed.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.installed.json" \
    || fail "installed Tasks, Actions, Budgets, or provider metadata changed"
budget_snapshot "$state_directory/state.db" "$workspace/budget.installed.json"
cmp -s "$workspace/budget.before.json" "$workspace/budget.installed.json" \
    || fail "installed provider budget changed"
budget_after=$(provider_budget_rows "$state_directory/state.db")
printf 'provider_budget_rows_after=%s\n' "$budget_after"
[ "$budget_after" = "$budget_before" ] \
    || fail "installed provider budget row count changed"
verify_quiescent_v3 "$rollback/state.db" \
    || fail "retained rollback tree is not the original quiescent v3 state"
logical_snapshot "$rollback/state.db" "$workspace/logical.rollback-retained.json"
cmp -s "$workspace/logical.before.json" \
    "$workspace/logical.rollback-retained.json" \
    || fail "retained v3 rollback tree changed"
budget_snapshot "$rollback/state.db" "$workspace/budget.rollback-retained.json"
cmp -s "$workspace/budget.before.json" \
    "$workspace/budget.rollback-retained.json" \
    || fail "retained v3 provider budget changed"
archive_is_unchanged || fail "backup or checksum changed after swap"
service_must_be_inactive

swap_complete=1
phase=complete
flock -u 8
exec 8>&-
flock -u 9
exec 9>&-
trap - EXIT HUP INT TERM
rm -rf "$workspace"

printf '\n==> staged schema-v4 deployment preparation complete\n'
printf 'backup=%s\n' "$archive"
printf 'rollback_directory=%s\n' "$rollback"
printf 'failed_state_directory_if_needed=%s\n' "$failed_state"
printf 'service_state=inactive\n'
printf 'wake_capability_active=false\n'
printf 'gaudere staged schema v4 deployment: PREPARED\n'
printf 'PREP ONLY / NOT AUTHORIZED FOR PRODUCTION; service remains stopped.\n'
