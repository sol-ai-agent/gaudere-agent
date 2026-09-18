#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)

expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_production_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-9bae154c1c47ea4351e452851874f65c84dc132970f7307b6cf4cde40d61010e}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
model_selector=/$model_id
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
goose_root=${GAUDERE_GOOSE_ROOT:-"$data_home/gaudere/goose"}
state_database=$state_directory/state.db
activity_sidecar=$state_directory/local-activity-pulse.db
governance_sidecar=$state_directory/goose-governance.db
model_registry=$goose_root/data/models/registry.json

fail()
{
    printf 'gaudere Local Goose stage5 proof: FAIL: %s\n' "$*" >&2
    exit 1
}

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sha256sum tar cp mktemp sed awk grep dirname rm mkdir; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

[ -f "$state_database" ] && [ ! -L "$state_database" ] || fail "production state database is missing or unsafe"
[ -f "$activity_sidecar" ] && [ ! -L "$activity_sidecar" ] || fail "local activity sidecar is missing or unsafe"
[ -f "$governance_sidecar" ] && [ ! -L "$governance_sidecar" ] || fail "Goose governance sidecar is missing or unsafe"
[ -f "$model_registry" ] && [ ! -L "$model_registry" ] || fail "Goose model registry is missing or unsafe"

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to the gaudere-agent checkout"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")

[ "$($systemctl_command --user is-active gaudere-agent.service)" = "active" ] || fail "production gaudere-agent service is not active"
production_image=$($podman_command inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://')
[ "$production_image" = "$expected_production_image" ] || fail "production image differs from expected immutable image"

provider_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total before proof is $provider_before, expected $expected_provider_total"

proof_root=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-local-goose-stage5.XXXXXX")
proof_runtime=$proof_root/runtime
proof_goose=$proof_root/goose
mkdir -p "$proof_runtime" "$proof_goose"
proof_image="localhost/gaudere-local-goose-stage5:${agent_ref}"
proof_container="gaudere-local-goose-stage5-$$"
proof_status=1

cleanup()
{
    rc=$?
    "$podman_command" rm -f "$proof_container" >/dev/null 2>&1 || true
    if [ "$rc" -eq 0 ] && [ "$proof_status" -eq 0 ]; then
        "$podman_command" rmi "$proof_image" >/dev/null 2>&1 || true
        rm -rf "$proof_root"
    else
        printf 'gaudere Local Goose stage5 proof: diagnostic directory preserved: %s\n' "$proof_root" >&2
    fi
}
trap cleanup EXIT
trap 'proof_status=1; exit 130' HUP INT TERM

python3 - "$state_database" "$activity_sidecar" "$governance_sidecar" "$proof_runtime" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

state, activity, governance, root = map(Path, sys.argv[1:])

def backup(source: Path, destination: Path) -> None:
    source_db = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    try:
        destination_db = sqlite3.connect(destination)
        try:
            source_db.backup(destination_db)
        finally:
            destination_db.close()
    finally:
        source_db.close()
    os.chmod(destination, 0o600)

backup(state, root / "state.db")
backup(activity, root / "local-activity-pulse.db")
backup(governance, root / "goose-governance.db")
PY

metadata_tar=$proof_root/goose-metadata.tar
tar -C "$goose_root" --exclude='./cache' -cf "$metadata_tar" .
tar -C "$proof_goose" -xf "$metadata_tar"
rm -f "$metadata_tar"
mkdir -p "$proof_goose/cache"

registered_relative=$(python3 - "$goose_root" "$model_registry" "$model_id" "$model_sha256" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
registry = Path(sys.argv[2])
model_id = sys.argv[3]
expected = sys.argv[4]
container_root = "/var/lib/gaudere/goose/"
document = json.loads(registry.read_text(encoding="utf-8"))
entries = [entry for entry in document.get("models", []) if isinstance(entry, dict) and entry.get("id") == model_id]
if len(entries) != 1:
    raise SystemExit(f"expected exactly one registry entry for {model_id}, found {len(entries)}")
entry = entries[0]
if entry.get("shard_files"):
    raise SystemExit("stage5 proof requires the existing single-file GGUF registration")
registered = entry.get("local_path")
if not isinstance(registered, str) or not registered.startswith(container_root):
    raise SystemExit(f"non-canonical Goose local_path: {registered!r}")
relative = registered[len(container_root):]
source = root / relative
if not source.is_file():
    raise SystemExit(f"registered GGUF missing: {source}")
hash_value = hashlib.sha256()
with source.open("rb") as stream:
    for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
        hash_value.update(block)
actual = hash_value.hexdigest()
if actual != expected:
    raise SystemExit(f"model SHA256 differs: {actual}")
print(relative)
PY
)

[ -n "$registered_relative" ] || fail "could not resolve registered model path"
model_source=$goose_root/$registered_relative
model_copy=$proof_goose/$registered_relative
mkdir -p "$(dirname -- "$model_copy")"
cp --reflink=auto --sparse=always "$model_source" "$model_copy"
[ "$(sha256sum "$model_copy" | awk '{print $1}')" = "$model_sha256" ] || fail "copied model SHA256 differs"

python3 - "$proof_runtime/state.db" "$proof_runtime/local-activity-pulse.db" "$proof_runtime/local-goose-cycle.db" <<'PY'
import hashlib
import os
from pathlib import Path
import sqlite3
import sys
import time

state_path, activity_path, cycle_path = map(Path, sys.argv[1:])
activity = sqlite3.connect(activity_path)
row = activity.execute(
    "SELECT generation,state,task_id,result_sha256 FROM local_activity_pulse_cursor "
    "WHERE scope=?",
    ("continuity.local-observation-pulse.v1",),
).fetchone()
activity.close()
if row is None or row[0] != 3 or row[1] != 4 or not row[2] or not row[3]:
    raise SystemExit("production-copy local activity cursor is not canonical generation-3 quiescent")
anchor_task_id, anchor_result_sha256 = row[2], row[3]
if not anchor_task_id.startswith("continuity.local-observation.v1:") or len(anchor_result_sha256) != 64:
    raise SystemExit("final observation identity is not canonical")

state = sqlite3.connect(state_path)
task = state.execute(
    "SELECT kind,status,result_output,result_failure_code,result_failure_message FROM tasks WHERE id=?",
    (anchor_task_id,),
).fetchone()
state.close()
if task is None or task[0] != "continuity.local-observation.v1" or task[1] != 3:
    raise SystemExit("final observation Task is missing or not succeeded in copied state")
if (task[3] or "") != "" or (task[4] or "") != "" or task[2] is None:
    raise SystemExit("final observation Task result is not a clean success")
actual_anchor_hash = hashlib.sha256(task[2].encode("utf-8")).hexdigest()
if actual_anchor_hash != anchor_result_sha256:
    raise SystemExit("final observation result hash differs from quiescent cursor")

due_at_ms = int(time.time() * 1000)
cycle = sqlite3.connect(cycle_path)
cycle.execute("PRAGMA journal_mode=DELETE")
cycle.execute("PRAGMA synchronous=FULL")
cycle.execute(
    "CREATE TABLE local_goose_cycle_cursor ("
    "scope TEXT PRIMARY KEY NOT NULL,"
    "revision INTEGER NOT NULL CHECK(revision >= 0),"
    "generation INTEGER NOT NULL CHECK(generation >= 0),"
    "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),"
    "anchor_observation_task_id TEXT NOT NULL,"
    "anchor_observation_result_sha256 TEXT NOT NULL,"
    "predecessor_task_id TEXT,"
    "predecessor_result_sha256 TEXT,"
    "due_at_ms INTEGER CHECK(due_at_ms IS NULL OR due_at_ms >= 0),"
    "captured_at_ms INTEGER CHECK(captured_at_ms IS NULL OR captured_at_ms >= 0),"
    "current_task_id TEXT NOT NULL,"
    "blocked_reason TEXT NOT NULL)"
)
cycle.execute("PRAGMA user_version=1")
cycle.execute(
    "INSERT INTO local_goose_cycle_cursor VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
    (
        "cognition.local-goose-cycle.v1", 1, 1, 1,
        anchor_task_id, anchor_result_sha256,
        None, None, due_at_ms, None, "", "",
    ),
)
cycle.commit()
cycle.close()
os.chmod(cycle_path, 0o600)
print(f"STAGE5_ANCHOR={anchor_task_id}")
print(f"STAGE5_DUE_AT_MS={due_at_ms}")
PY

printf '=== BUILD CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$proof_image" sh "$script_directory/build-image.sh"

"$podman_command" run --rm --network=none --entrypoint /usr/bin/test "$proof_image" -x /usr/local/bin/gaudere-local-goose-cycle-runtime
"$podman_command" run --rm --network=none --entrypoint /usr/local/bin/gaudere-local-goose-cycle-runtime "$proof_image" --help \
    | grep -q -- '--once'

printf '\n=== REAL LOCAL GOOSE CYCLE / NETWORK NONE ===\n'
if "$podman_command" run --name "$proof_container" --rm \
    --network=none \
    --userns=keep-id \
    --memory=12G \
    --pids-limit=64 \
    --read-only \
    --read-only-tmpfs \
    --security-opt=no-new-privileges \
    --cap-drop=all \
    --volume "$proof_runtime:/work:Z" \
    --volume "$proof_goose:/var/lib/gaudere/goose:Z" \
    --entrypoint /usr/local/bin/gaudere-local-goose-cycle-runtime \
    "$proof_image" \
    --state /work/state.db \
    --cycle-sidecar /work/local-goose-cycle.db \
    --model "$model_selector" \
    --model-sha256 "$model_sha256" \
    --governance /work/goose-governance.db \
    --control-socket /work/control.sock \
    --once \
    >"$proof_runtime/runtime.log" 2>&1; then
    runtime_rc=0
else
    runtime_rc=$?
fi
cat "$proof_runtime/runtime.log"
[ "$runtime_rc" -eq 0 ] || fail "isolated runtime returned $runtime_rc"
grep -q 'provider_execution=false automatic_seed=false' "$proof_runtime/runtime.log" || fail "provider-free runtime marker missing"
grep -q 'gaudere-local-goose-cycle-runtime: decision=' "$proof_runtime/runtime.log" || fail "no canonical local decision was emitted"
grep -q 'gaudere-local-goose-cycle-runtime: once=complete' "$proof_runtime/runtime.log" || fail "one-cycle completion marker missing"
grep -q 'gaudere-local-goose-cycle-runtime: safe' "$proof_runtime/log" || fail "safe shutdown marker missing"
[ ! -e "$proof_runtime/control.sock" ] || fail "control socket remained after runtime shutdown"

python3 - "$proof_runtime/state.db" "$proof_runtime/local-goose-cycle.db" "$expected_provider_total" <<'PY'
from pathlib import Path
import sqlite3
import sys

state_path, cycle_path = map(Path, sys.argv[1:3])
expected_provider = int(sys.argv[3])
state = sqlite3.connect(state_path)
provider = state.execute(
    "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses'"
).fetchone()[0]
cycles = state.execute(
    "SELECT id,status,attempts_started,result_output FROM tasks WHERE kind='cognition.local-goose-cycle.v1'"
).fetchall()
state.close()
if provider != expected_provider:
    raise SystemExit(f"copied-state provider total changed to {provider}")
if len(cycles) != 1 or cycles[0][1] != 3 or cycles[0][2] != 1 or not cycles[0][3]:
    raise SystemExit(f"expected one succeeded first-generation cycle Task, got {cycles!r}")
cycle = sqlite3.connect(cycle_path)
cursor = cycle.execute(
    "SELECT generation,state,predecessor_task_id,predecessor_result_sha256 FROM local_goose_cycle_cursor "
    "WHERE scope='cognition.local-goose-cycle.v1'"
).fetchone()
cycle.close()
if cursor is None or cursor[0] != 2 or cursor[1] not in (0, 1) or cursor[2] != cycles[0][0] or not cursor[3]:
    raise SystemExit(f"cycle cursor did not settle canonical generation 1: {cursor!r}")
print(f"STAGE5_TASK={cycles[0][0]}")
print(f"STAGE5_NEXT_STATE={'dormant' if cursor[1] == 0 else 'scheduled'}")
PY

printf '\n=== PRODUCTION PRESERVATION ===\n'
[ "$($systemctl_command --user is-active gaudere-agent.service)" = "active" ] || fail "production service stopped during proof"
production_image_after=$($podman_command inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://')
[ "$production_image_after" = "$expected_production_image" ] || fail "production image changed during proof"
provider_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$provider_after" = "$expected_provider_total" ] || fail "production provider total changed to $provider_after"

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'MODEL_ID=%s\n' "$model_id"
printf 'MODEL_SHA256=%s\n' "$model_sha256"
printf 'PRODUCTION_IMAGE=%s\n' "$production_image_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'FEDORA_LOCAL_GOOSE_CYCLE_STAGE5=PASS\n'
proof_status=0
