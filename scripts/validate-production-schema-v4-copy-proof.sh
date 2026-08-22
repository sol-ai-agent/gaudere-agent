#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
state_dir=${GAUDERE_STATE_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"}
backup_dir=${GAUDERE_BACKUP_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/backups"}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-}
representative_task=${GAUDERE_REPRESENTATIVE_TASK:-production-initiative-first}
expected_budget_rows=${GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS:-3}
request_id=${GAUDERE_PROOF_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
candidate_image=${GAUDERE_CANDIDATE_IMAGE:-"localhost/gaudere-agent:schema-v4-proof-$request_id"}
rollback_image=${GAUDERE_ROLLBACK_IMAGE:-"localhost/gaudere-agent:rollback-pre-v4-$request_id"}
rollback_manifest=${GAUDERE_ROLLBACK_MANIFEST:-"$backup_dir/image-rollback-$request_id.manifest"}
test_mode=${GAUDERE_TEST_MODE:-0}
default_control_script="$script_directory/control-service.sh"
default_capture_script="$script_directory/capture-schema-v4-image-rollback.sh"
default_build_script="$script_directory/build-image.sh"
default_backup_script="$script_directory/backup-state.sh"
default_image_proof_script="$script_directory/validate-schema-v4-image-provenance.sh"
control_script=${GAUDERE_CONTROL_SCRIPT:-$default_control_script}
capture_script=${GAUDERE_CAPTURE_ROLLBACK_SCRIPT:-$default_capture_script}
build_script=${GAUDERE_BUILD_IMAGE_SCRIPT:-$default_build_script}
backup_script=${GAUDERE_BACKUP_SCRIPT:-$default_backup_script}
image_proof_script=${GAUDERE_IMAGE_PROOF_SCRIPT:-$default_image_proof_script}

fail()
{
    printf 'gaudere production schema-v4 copy proof: %s\n' "$*" >&2
    exit 1
}

need_command()
{
    command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

need_file()
{
    [ -f "$1" ] && [ ! -L "$1" ] || fail "required helper is missing or unsafe: $1"
}

snapshot_state()
{
    output=$1
    python3 - "$state_dir" > "$output" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
for name in sorted(os.listdir(root)):
    if name == "state.db.lock":
        continue
    path = os.path.join(root, name)
    if not os.path.isfile(path):
        continue
    h = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    print(f"{name}\t{os.path.getsize(path)}\t{h.hexdigest()}")
PY
}

service_was_stopped=0
before_snapshot=
after_snapshot=
cleanup()
{
    if [ "$service_was_stopped" = "1" ]; then
        "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
    fi
    [ -z "$before_snapshot" ] || rm -f "$before_snapshot"
    [ -z "$after_snapshot" ] || rm -f "$after_snapshot"
}
trap cleanup EXIT HUP INT TERM

[ "$#" -eq 0 ] || fail "usage: $0"
case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac
if [ "$test_mode" != "1" ]; then
    [ "$control_script" = "$default_control_script" ] \
        || fail "GAUDERE_CONTROL_SCRIPT override is restricted to synthetic test mode"
    [ "$capture_script" = "$default_capture_script" ] \
        || fail "GAUDERE_CAPTURE_ROLLBACK_SCRIPT override is restricted to synthetic test mode"
    [ "$build_script" = "$default_build_script" ] \
        || fail "GAUDERE_BUILD_IMAGE_SCRIPT override is restricted to synthetic test mode"
    [ "$backup_script" = "$default_backup_script" ] \
        || fail "GAUDERE_BACKUP_SCRIPT override is restricted to synthetic test mode"
    [ "$image_proof_script" = "$default_image_proof_script" ] \
        || fail "GAUDERE_IMAGE_PROOF_SCRIPT override is restricted to synthetic test mode"
fi

need_command git
need_command "$podman_command"
need_command "$systemctl_command"
need_command python3
need_command mktemp
need_command cmp
need_command sha256sum
need_command grep
need_command sed
need_command cat
need_file "$control_script"
need_file "$capture_script"
need_file "$build_script"
need_file "$backup_script"
need_file "$image_proof_script"
[ -d "$state_dir" ] || fail "production state directory not found: $state_dir"
[ -f "$state_dir/state.db" ] || fail "production state database not found"
mkdir -p "$backup_dir"

[ -n "$expected_agent_ref" ] || fail "GAUDERE_EXPECTED_AGENT_REF is required"
[ -n "$expected_core_ref" ] || fail "GAUDERE_EXPECTED_CORE_REF is required"
case "$expected_agent_ref" in *[!0-9a-f]*|'') fail "invalid expected Agent ref" ;; esac
case "$expected_core_ref" in *[!0-9a-f]*|'') fail "invalid expected Core ref" ;; esac
[ "${#expected_agent_ref}" -eq 40 ] || fail "expected Agent ref must be 40 hex characters"
[ "${#expected_core_ref}" -eq 40 ] || fail "expected Core ref must be 40 hex characters"
case "$expected_budget_rows" in ''|*[!0-9]*) fail "GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS must be an integer" ;; esac

head=$(git -C "$repository_root" rev-parse HEAD)
[ "$head" = "$expected_agent_ref" ] || fail "Agent HEAD mismatch: expected $expected_agent_ref observed $head"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] \
    || fail "gaudere-agent checkout must be clean"
core=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
[ "$core" = "$expected_core_ref" ] || fail "Core pin mismatch: expected $expected_core_ref observed $core"

printf 'AGENT_REF=%s\n' "$head"
printf 'CORE_REF=%s\n' "$core"

printf '\n==> live preflight\n'
[ "$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)" = "active" ] \
    || fail "$service_name must be active before the proof"
before_budget=$(sh "$control_script" budget)
printf '%s\n' "$before_budget"
printf '%s\n' "$before_budget" | grep -qx 'provider_enabled=true' \
    || fail "provider capability is not enabled"
printf '%s\n' "$before_budget" | grep -qx "total_used=$expected_budget_rows" \
    || fail "unexpected durable provider total before proof"

printf '\n==> capture immutable rollback image before candidate build\n'
GAUDERE_ROLLBACK_IMAGE="$rollback_image" \
GAUDERE_ROLLBACK_MANIFEST="$rollback_manifest" \
    sh "$capture_script"
rollback_id=$(sed -n 's/^rollback_image_id=//p' "$rollback_manifest")
[ -n "$rollback_id" ] || fail "rollback manifest did not contain an image ID"
printf 'ROLLBACK_ID=%s\n' "$rollback_id"

printf '\n==> build distinct exact candidate image\n'
GAUDERE_IMAGE_TAG="$candidate_image" sh "$build_script"
candidate_id=$("$podman_command" image inspect --format '{{.Id}}' "$candidate_image") \
    || fail "cannot inspect candidate image"
[ -n "$candidate_id" ] || fail "candidate image ID is empty"
printf 'CANDIDATE_ID=%s\n' "$candidate_id"

before_snapshot=$(mktemp "${TMPDIR:-/tmp}/gaudere-real-before.XXXXXX")
after_snapshot=$(mktemp "${TMPDIR:-/tmp}/gaudere-real-after.XXXXXX")

printf '\n==> stop service only for coherent backup and byte-identity proof\n'
"$systemctl_command" --user stop "$service_name"
service_was_stopped=1
[ "$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)" = "inactive" ] \
    || fail "$service_name did not become inactive"

snapshot_state "$before_snapshot"
printf 'REAL_STATE_SNAPSHOT_BEFORE:\n'
cat "$before_snapshot"

backup=$(GAUDERE_STATE_DIR="$state_dir" GAUDERE_BACKUP_DIR="$backup_dir" \
    sh "$backup_script")
printf 'BACKUP=%s\n' "$backup"
(
    cd "$(dirname "$backup")"
    sha256sum -c "$(basename "$backup").sha256"
)

printf '\n==> exact candidate/rollback provenance plus disposable v3-to-v4 proof\n'
GAUDERE_CANDIDATE_IMAGE="$candidate_image" \
GAUDERE_EXPECTED_AGENT_REF="$expected_agent_ref" \
GAUDERE_EXPECTED_CORE_REF="$expected_core_ref" \
GAUDERE_EXPECTED_CANDIDATE_ID="$candidate_id" \
GAUDERE_ROLLBACK_IMAGE="$rollback_image" \
GAUDERE_EXPECTED_ROLLBACK_ID="$rollback_id" \
GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS="$expected_budget_rows" \
    sh "$image_proof_script" "$backup" "$representative_task"

snapshot_state "$after_snapshot"
printf 'REAL_STATE_SNAPSHOT_AFTER:\n'
cat "$after_snapshot"
cmp -s "$before_snapshot" "$after_snapshot" \
    || fail "real production state changed during disposable proof"
printf 'REAL_STATE_BYTE_IDENTITY=PASS\n'

python3 - "$state_dir/state.db" "$expected_budget_rows" <<'PY'
import sqlite3
import sys

path = sys.argv[1]
expected_budget_rows = int(sys.argv[2])
with sqlite3.connect(f"file:{path}?mode=ro", uri=True) as db:
    schema = db.execute("PRAGMA user_version").fetchone()[0]
    wake_objects = db.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE name='wake_intents'"
    ).fetchone()[0]
    budget_rows = db.execute("SELECT COUNT(*) FROM budget_consumptions").fetchone()[0]
    integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
    print(f"REAL_SCHEMA_AFTER_PROOF={schema}")
    print(f"REAL_WAKE_OBJECTS_AFTER_PROOF={wake_objects}")
    print(f"REAL_PROVIDER_BUDGET_ROWS_AFTER_PROOF={budget_rows}")
    print(f"REAL_INTEGRITY_AFTER_PROOF={integrity}")
    assert schema == 3
    assert wake_objects == 0
    assert budget_rows == expected_budget_rows
    assert integrity == "ok"
PY

printf '\n==> restart unchanged production service\n'
"$systemctl_command" --user start "$service_name"
sleep 1
[ "$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)" = "active" ] \
    || fail "$service_name is not active after proof"
service_was_stopped=0
printf 'REAL_SERVICE_AFTER_PROOF=active\n'

after_budget=$(sh "$control_script" budget)
printf '%s\n' "$after_budget"
printf '%s\n' "$after_budget" | grep -qx "total_used=$expected_budget_rows" \
    || fail "durable provider total changed during provider-free proof"

printf '\nREPRESENTATIVE_TASK_AFTER_PROOF:\n'
sh "$control_script" task "$representative_task"

printf '\ngaudere production schema-v4 copy proof: PASS\n'
