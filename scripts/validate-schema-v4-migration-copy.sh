#!/bin/sh
set -eu

archive=${1:-}
task_id=${2:-}
podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
agent_bin=${GAUDERE_AGENT_BIN:-}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root=${GAUDERE_VALIDATION_ROOT:-"$data_home/gaudere/validation"}
expected_provider_budget_rows=${GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS:-}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
required_agent_base=a5a5fbb27af85faf584318bf8ddcfa290d3df5ad
required_core_ref=c24c40b84a12e51515cee4611e3dc79e9fd83892

fail()
{
    printf 'gaudere schema v4 migration proof: %s\n' "$*" >&2
    exit 1
}

[ -n "$archive" ] \
    || fail "usage: $0 BACKUP_ARCHIVE [REPRESENTATIVE_TASK_ID]"
[ -f "$archive" ] || fail "backup archive not found: $archive"
checksum="$archive.sha256"
[ -f "$checksum" ] \
    || fail "verified backup checksum is required: $checksum"
[ ! -L "$archive" ] || fail "backup archive must not be a symbolic link"
[ ! -L "$checksum" ] || fail "backup checksum must not be a symbolic link"

case "$expected_provider_budget_rows" in
    ''|*[!0-9]*)
        [ -z "$expected_provider_budget_rows" ] \
            || fail "GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS must be a non-negative integer"
        ;;
esac

for command in cmp cp flock git mktemp python3 realpath sha256sum tar; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done

git -C "$repository_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || fail "validator must run from a Gaudere Agent Git checkout"
git -C "$repository_root" cat-file -e "$required_agent_base^{commit}" \
    || fail "required Agent capability commit is unavailable"
git -C "$repository_root" merge-base --is-ancestor \
    "$required_agent_base" HEAD \
    || fail "checkout does not contain merged Agent exact-wake gate"
agent_source_head=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
[ "$core_ref" = "$required_core_ref" ] \
    || fail "unexpected Gaudere Core ref: $core_ref"
printf 'agent_source_head=%s\n' "$agent_source_head"
printf 'required_agent_base=%s\n' "$required_agent_base"
printf 'core_ref=%s\n' "$core_ref"

if [ -n "$agent_bin" ]; then
    [ -x "$agent_bin" ] || fail "Agent binary is not executable: $agent_bin"
else
    command -v "$podman_command" >/dev/null 2>&1 \
        || fail "required command not found: $podman_command"
    "$podman_command" image exists "$image" \
        || fail "image does not exist: $image"
fi

archive=$(realpath "$archive")
checksum=$(realpath "$checksum")
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/schema-v4-migration.XXXXXX")
migrated_state="$workspace/migrated-state"
rollback_state="$workspace/rollback-state"
private_archive="$workspace/source.tar.gz"
mkdir -p "$migrated_state" "$rollback_state"

cleanup()
{
    if [ "${KEEP_GAUDERE_VALIDATION_STATE:-0}" = "1" ]; then
        printf 'schema v4 migration proof state kept at %s\n' "$workspace" >&2
    else
        rm -rf "$workspace"
    fi
}
trap cleanup EXIT HUP INT TERM

if ! python3 - "$archive" "$checksum" <<'PY'
import hashlib
import pathlib
import re
import sys

archive = pathlib.Path(sys.argv[1])
checksum = pathlib.Path(sys.argv[2])
try:
    lines = checksum.read_text(encoding="ascii").splitlines()
except (OSError, UnicodeError) as error:
    raise SystemExit(f"could not read checksum manifest: {error}")
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
PY
then
    fail "backup checksum verification failed"
fi
sha256sum "$archive" > "$workspace/archive.before.sha256"
cp "$archive" "$private_archive"
cmp -s "$archive" "$private_archive" \
    || fail "private archive copy differs from verified source"

if ! python3 - "$private_archive" <<'PY'
import pathlib
import sys
import tarfile

archive = sys.argv[1]
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
then
    fail "backup archive safety validation failed"
fi

printf '\n==> restore verified archive into two disposable states\n'
tar --no-same-owner --no-same-permissions \
    -xzf "$private_archive" -C "$migrated_state"
tar --no-same-owner --no-same-permissions \
    -xzf "$private_archive" -C "$rollback_state"
[ -f "$migrated_state/state.db" ] || fail "migrated copy has no state.db"
[ -f "$rollback_state/state.db" ] || fail "rollback copy has no state.db"
[ ! -e "$migrated_state/state.db.lock" ] \
    || fail "migrated copy unexpectedly contains state.db.lock"
[ ! -e "$rollback_state/state.db.lock" ] \
    || fail "rollback copy unexpectedly contains state.db.lock"

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
    integrity = [row[0] for row in db.execute("PRAGMA integrity_check")]
    if integrity != ["ok"]:
        raise SystemExit("SQLite integrity_check failed")

    objects = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' "
        "AND tbl_name!='wake_intents' ORDER BY type,name"
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

provider_budget_rows()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    exists = db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' "
        "AND name='budget_consumptions'"
    ).fetchone()
    if not exists:
        print(0)
    else:
        print(db.execute(
            "SELECT COUNT(*) FROM budget_consumptions WHERE scope=?",
            ("provider.call:openai.responses",),
        ).fetchone()[0])
PY
}

verify_no_wake_table()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse

uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    count = db.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE name='wake_intents'"
    ).fetchone()[0]
    if count != 0:
        raise SystemExit("schema-v3 rollback state unexpectedly contains wake_intents")
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
        raise SystemExit("wake_intents schema objects do not match schema v4")
    rows = db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0]
    if rows != 0:
        raise SystemExit("migration fabricated a wake intent")
    print(rows)
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

migrated_db="$migrated_state/state.db"
rollback_db="$rollback_state/state.db"
before_version=$(schema_version "$migrated_db")
printf 'schema_before=%s\n' "$before_version"
[ "$before_version" = "3" ] \
    || fail "expected source schema 3, found $before_version"
verify_no_wake_table "$migrated_db"
logical_snapshot "$migrated_db" "$workspace/logical.before.json"

budget_before=$(provider_budget_rows "$migrated_db")
printf 'provider_budget_rows_before=%s\n' "$budget_before"
if [ -n "$expected_provider_budget_rows" ]; then
    [ "$budget_before" = "$expected_provider_budget_rows" ] \
        || fail "expected $expected_provider_budget_rows provider budget rows, found $budget_before"
fi

printf '\n==> prove the shared state lock blocks migration\n'
lock_output="$workspace/locked.out"
lock_error="$workspace/locked.err"
if ! (
    exec 9>>"$migrated_db.lock"
    flock -n 9 || exit 91
    if run_agent_on "$migrated_state" --check --wake-intents \
        >"$lock_output" 2>"$lock_error"; then
        exit 92
    fi
); then
    fail "could not prove lock refusal on the disposable copy"
fi
grep -q 'state database is already owned' "$lock_error" \
    || fail "locked migration did not report the existing owner"
printf 'lock_refusal=PASS\n'

printf '\n==> migrate only the disposable copy with exact wake explicitly enabled\n'
migration_output=$(run_agent_on "$migrated_state" --check --wake-intents)
printf '%s\n' "$migration_output"
printf '%s\n' "$migration_output" | grep -q \
    '^gaudere-agent: explicit wake enabled scope=cognition.reflect.wake.v0 max_total=1 automatic_successor=false$' \
    || fail "migration did not activate the fixed wake capability"
printf '%s\n' "$migration_output" | grep -q '^gaudere-agent: running$' \
    || fail "migration check did not reach readiness"
printf '%s\n' "$migration_output" | grep -q '^gaudere-agent: safe$' \
    || fail "migration check did not finish safe"

after_version=$(schema_version "$migrated_db")
printf 'schema_after=%s\n' "$after_version"
[ "$after_version" = "4" ] \
    || fail "expected migrated schema 4, found $after_version"
logical_snapshot "$migrated_db" "$workspace/logical.after.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.after.json" \
    || fail "pre-existing schema or logical rows changed during migration"
wake_rows=$(verify_empty_wake_schema "$migrated_db")
printf 'wake_rows=%s\n' "$wake_rows"
budget_after=$(provider_budget_rows "$migrated_db")
printf 'provider_budget_rows_after=%s\n' "$budget_after"
[ "$budget_after" = "$budget_before" ] \
    || fail "provider budget changed during migration"

if [ -n "$task_id" ]; then
    printf '\n==> inspect representative durable task after migration\n'
    run_agent_on "$migrated_state" --task "$task_id"
fi

printf '\n==> prove schema-v4 reopen is idempotent and default mode remains inert\n'
reopen_output=$(run_agent_on "$migrated_state" --check --wake-intents)
printf '%s\n' "$reopen_output" | grep -q '^gaudere-agent: safe$' \
    || fail "schema-v4 opt-in reopen did not finish safe"
logical_snapshot "$migrated_db" "$workspace/logical.reopen.json"
cmp -s "$workspace/logical.before.json" "$workspace/logical.reopen.json" \
    || fail "schema-v4 opt-in reopen changed pre-existing durable state"
[ "$(verify_empty_wake_schema "$migrated_db")" = "0" ] \
    || fail "schema-v4 reopen fabricated a wake"

default_output=$(run_agent_on "$migrated_state" --check)
printf '%s\n' "$default_output" | grep -q '^gaudere-agent: safe$' \
    || fail "default schema-v4 reopen did not finish safe"
if printf '%s\n' "$default_output" | grep -q 'explicit wake enabled'; then
    fail "default reopen unexpectedly enabled exact wake"
fi
[ "$(schema_version "$migrated_db")" = "4" ] \
    || fail "default reopen changed schema-v4 version"
logical_snapshot "$migrated_db" "$workspace/logical.default-reopen.json"
cmp -s "$workspace/logical.before.json" \
    "$workspace/logical.default-reopen.json" \
    || fail "default schema-v4 reopen changed pre-existing durable state"
printf 'v4_reopen=PASS\n'

printf '\n==> prove the untouched archive restores the independent schema-v3 rollback\n'
rollback_version=$(schema_version "$rollback_db")
printf 'rollback_schema_before=%s\n' "$rollback_version"
[ "$rollback_version" = "3" ] \
    || fail "rollback copy does not preserve schema 3"
verify_no_wake_table "$rollback_db"
logical_snapshot "$rollback_db" "$workspace/logical.rollback-before.json"
cmp -s "$workspace/logical.before.json" \
    "$workspace/logical.rollback-before.json" \
    || fail "rollback copy differs from the source logical state"

rollback_output=$(run_agent_on "$rollback_state" --check)
printf '%s\n' "$rollback_output" | grep -q '^gaudere-agent: safe$' \
    || fail "schema-v3 rollback check did not finish safe"
[ "$(schema_version "$rollback_db")" = "3" ] \
    || fail "default rollback check migrated schema 3"
verify_no_wake_table "$rollback_db"
logical_snapshot "$rollback_db" "$workspace/logical.rollback-after.json"
cmp -s "$workspace/logical.before.json" \
    "$workspace/logical.rollback-after.json" \
    || fail "schema-v3 rollback check changed durable state"
if [ -n "$task_id" ]; then
    run_agent_on "$rollback_state" --task "$task_id" >/dev/null
fi
printf 'rollback_restore=PASS\n'

printf '\n==> prove the source archive remained immutable\n'
sha256sum "$archive" > "$workspace/archive.after.sha256"
cmp -s "$workspace/archive.before.sha256" "$workspace/archive.after.sha256" \
    || fail "source backup archive changed during validation"
cmp -s "$archive" "$private_archive" \
    || fail "private validation copy changed the source archive"
printf 'archive_immutable=PASS\n'

printf '\n==> schema-v4 disposable migration and rollback proof complete\n'
printf 'gaudere schema v4 migration copy validation: PASS\n'
