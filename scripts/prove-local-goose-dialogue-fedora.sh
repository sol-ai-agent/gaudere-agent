#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_production_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-321df97f8b03e12de1727ad13c5e9d05646fb807e52fbef9093fc61127b98595}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
model_selector=/$model_id
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
goose_root=${GAUDERE_GOOSE_ROOT:-"$data_home/gaudere/goose"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
model_registry="$goose_root/data/models/registry.json"
proof_root="$data_home/gaudere/.local-goose-dialogue-proof"
workspace=""
proof_image=""
container_name="gaudere-local-goose-dialogue-proof-$$"
container_started=0
proof_ok=0

fail()
{
    printf 'gaudere Local Goose dialogue Fedora proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$container_started" = "1" ]; then
        "$podman_command" rm -f "$container_name" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$proof_image" ]; then
        "$podman_command" image rm "$proof_image" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'gaudere Local Goose dialogue Fedora proof: diagnostic directory preserved: %s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sha256sum tar cp mktemp sed awk grep dirname rm mkdir chmod id sleep; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$model_registry"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
short_ref=$(printf '%s\n' "$agent_ref" | cut -c1-12)
host_uid=$(id -u)
host_gid=$(id -g)
proof_image="localhost/gaudere-agent:dialogue-proof-$short_ref"

[ "$("$systemctl_command" --user is-active "$service_name")" = "active" ] || fail "production service is not active"
production_image_before=$("$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://')
[ "$production_image_before" = "$expected_production_image" ] || fail "production image differs from expected immutable image"
production_network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$production_network_before" = "none" ] || fail "production network is not none"

cursor_query="SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';"
production_cursor_before=$(sqlite3 -readonly "$cycle_sidecar" "$cursor_query")
[ -n "$production_cursor_before" ] || fail "production Local Goose cycle cursor is missing"

provider_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total is $provider_before, expected $expected_provider_total"

production_cycle_tasks_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
production_dialogue_tasks_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';")
[ "$production_dialogue_tasks_before" = "0" ] || fail "production already contains Local Goose dialogue Tasks"
production_stimuli_before=$(sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")
[ "$production_stimuli_before" = "0" ] || fail "production stimulus ledger is not empty"

mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/run.XXXXXX")
runtime_dir="$workspace/runtime"
goose_copy="$workspace/goose"
mkdir -p "$runtime_dir" "$goose_copy"

python3 - "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$runtime_dir" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

state, cycle, stimulus, root = map(Path, sys.argv[1:])

def backup(source: Path, destination: Path) -> None:
    src = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    try:
        dst = sqlite3.connect(destination)
        try:
            src.backup(dst)
        finally:
            dst.close()
    finally:
        src.close()
    os.chmod(destination, 0o600)

backup(state, root / "state.db")
backup(cycle, root / "local-goose-cycle.db")
backup(stimulus, root / "local-goose-cycle-stimulus.db")
PY

copy_cursor_before=$(sqlite3 -readonly "$runtime_dir/local-goose-cycle.db" "$cursor_query")
[ "$copy_cursor_before" = "$production_cursor_before" ] || fail "copied cycle cursor differs from production"
copy_provider_before=$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$copy_provider_before" = "$provider_before" ] || fail "copied provider total differs"
copy_stimuli_before=$(sqlite3 -readonly "$runtime_dir/local-goose-cycle-stimulus.db" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")
[ "$copy_stimuli_before" = "$production_stimuli_before" ] || fail "copied stimulus ledger differs"

metadata_tar="$workspace/goose-metadata.tar"
tar -C "$goose_root" --exclude='./cache' -cf "$metadata_tar" .
tar -C "$goose_copy" -xf "$metadata_tar"
rm -f "$metadata_tar"
mkdir -p "$goose_copy/cache"

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
entries = [e for e in document.get("models", []) if isinstance(e, dict) and e.get("id") == model_id]
if len(entries) != 1:
    raise SystemExit(f"expected one registry entry for {model_id}, found {len(entries)}")
entry = entries[0]
if entry.get("shard_files"):
    raise SystemExit("dialogue proof requires the existing single-file GGUF registration")
registered = entry.get("local_path")
if not isinstance(registered, str) or not registered.startswith(container_root):
    raise SystemExit(f"non-canonical Goose local_path: {registered!r}")
relative = registered[len(container_root):]
source = root / relative
if not source.is_file():
    raise SystemExit(f"registered GGUF missing: {source}")
digest = hashlib.sha256()
with source.open("rb") as stream:
    for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
        digest.update(block)
actual = digest.hexdigest()
if actual != expected:
    raise SystemExit(f"model SHA256 differs: {actual}")
print(relative)
PY
)

[ -n "$registered_relative" ] || fail "could not resolve registered local model"
model_source="$goose_root/$registered_relative"
model_copy="$goose_copy/$registered_relative"
mkdir -p "$(dirname -- "$model_copy")"
cp --reflink=auto --sparse=always "$model_source" "$model_copy"
[ "$(sha256sum "$model_copy" | awk '{print $1}')" = "$model_sha256" ] || fail "copied model SHA256 differs"

printf '=== BUILD DIALOGUE PROOF CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$proof_image" sh "$script_directory/build-image.sh"

"$podman_command" run --rm --network none --entrypoint /usr/bin/test "$proof_image" -x /usr/local/bin/gaudere-local-goose-dialogue-proof || fail "candidate lacks dialogue proof runtime"

printf '=== RUN ISOLATED LOCAL DIALOGUE / NETWORK NONE ===\n'
"$podman_command" run -d \
    --name "$container_name" \
    --network none \
    --userns=keep-id \
    --user "$host_uid:$host_gid" \
    --memory=12G \
    --pids-limit=64 \
    --read-only \
    --read-only-tmpfs \
    --security-opt=no-new-privileges \
    --cap-drop=all \
    --volume "$runtime_dir:/work:Z" \
    --volume "$goose_copy:/var/lib/gaudere/goose:Z" \
    --entrypoint /usr/local/bin/gaudere-local-goose-dialogue-proof \
    "$proof_image" \
    --state /work/state.db \
    --model "$model_selector" \
    --model-sha256 "$model_sha256" \
    --control-socket /work/control.sock >/dev/null
container_started=1

proof_network=$("$podman_command" inspect "$container_name" --format '{{.HostConfig.NetworkMode}}')
[ "$proof_network" = "none" ] || fail "proof container network is not none"

attempt=0
while [ "$attempt" -lt 200 ] && [ ! -S "$runtime_dir/control.sock" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
done
[ -S "$runtime_dir/control.sock" ] || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "dialogue proof control socket did not become ready"
}

request_id="fedora-dialogue-proof-$short_ref"
message="Bonjour Gaudere. Ceci est une preuve Fedora isolée. Réponds brièvement pour confirmer que le dialogue local fonctionne."
client_output=$("$podman_command" exec \
    --user "$host_uid:$host_gid" \
    "$container_name" \
    /usr/local/bin/gaudere-control \
    --socket /work/control.sock \
    local-message "$request_id" "$message") || {
        "$podman_command" logs "$container_name" >&2 || true
        fail "bounded local-message submission failed"
    }
printf '%s\n' "$client_output"
printf '%s\n' "$client_output" | grep -q '^kind="cognition.local-goose-dialogue.v1"$' || fail "local-message did not create dialogue Task"
printf '%s\n' "$client_output" | grep -q '^status=pending$' || fail "dialogue Task was not initially pending"

container_exit=$("$podman_command" wait "$container_name")
[ "$container_exit" = "0" ] || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "dialogue proof runtime exited with status $container_exit"
}

server_output=$("$podman_command" logs "$container_name")
printf '%s\n' "$server_output"
printf '%s\n' "$server_output" | grep -q '^FEDORA_LOCAL_GOOSE_DIALOGUE_RUNTIME=PASS$' || fail "dialogue proof runtime did not report PASS"
printf '%s\n' "$server_output" | grep -q 'provider_execution=false tools_enabled=false network_expected=none' || fail "dialogue proof authority marker missing"

python3 - "$runtime_dir/state.db" "$runtime_dir/local-goose-cycle.db" "$runtime_dir/local-goose-cycle-stimulus.db" "$request_id" "$model_sha256" "$copy_cursor_before" "$copy_provider_before" "$copy_stimuli_before" <<'PY'
import json
from pathlib import Path
import sqlite3
import sys

state_path = Path(sys.argv[1])
cycle_path = Path(sys.argv[2])
stimulus_path = Path(sys.argv[3])
request_id = sys.argv[4]
model_sha = sys.argv[5]
expected_cursor = sys.argv[6]
expected_provider = int(sys.argv[7])
expected_stimuli = int(sys.argv[8])

state = sqlite3.connect(state_path)
provider = state.execute(
    "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses'"
).fetchone()[0]
rows = state.execute(
    "SELECT id,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE kind='cognition.local-goose-dialogue.v1'"
).fetchall()
state.close()

if provider != expected_provider:
    raise SystemExit(f"copied provider total changed to {provider}")
if len(rows) != 1:
    raise SystemExit(f"expected exactly one dialogue Task, got {len(rows)}")
task_id, status, attempts, content_type, output, failure_code, failure_message = rows[0]
if status != 3 or attempts != 1:
    raise SystemExit(f"dialogue Task did not succeed exactly once: status={status} attempts={attempts}")
if content_type != "application/vnd.gaudere.local-goose-dialogue-response+json":
    raise SystemExit(f"dialogue result content type differs: {content_type!r}")
if failure_code or failure_message:
    raise SystemExit("dialogue Task contains failure evidence")
document = json.loads(output)
if set(document) != {"model_sha256", "request_id", "response", "schema"}:
    raise SystemExit("dialogue result keys differ")
if document["schema"] != "gaudere.cognition.local-goose-dialogue-response.v1":
    raise SystemExit("dialogue result schema differs")
if document["request_id"] != request_id or document["model_sha256"] != model_sha:
    raise SystemExit("dialogue response identity differs")
if not isinstance(document["response"], str) or not document["response"].strip():
    raise SystemExit("dialogue response is empty")
if len(document["response"].encode("utf-8")) > 16 * 1024:
    raise SystemExit("dialogue response exceeds bound")

cycle = sqlite3.connect(cycle_path)
cursor = cycle.execute(
    "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||"
    "COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||"
    "COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') "
    "FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1'"
).fetchone()
cycle.close()
if cursor is None or cursor[0] != expected_cursor:
    raise SystemExit("copied autonomous cycle changed during dialogue proof")

stimulus = sqlite3.connect(stimulus_path)
stimuli = stimulus.execute("SELECT COUNT(*) FROM local_goose_cycle_stimuli").fetchone()[0]
stimulus.close()
if stimuli != expected_stimuli:
    raise SystemExit("copied stimulus ledger changed during dialogue proof")

print(f"DIALOGUE_TASK={task_id}")
print(f"DIALOGUE_RESPONSE={document['response']}")
print(f"COPIED_PROVIDER_TOTAL={provider}")
print(f"COPIED_CYCLE_CURSOR={cursor[0]}")
print(f"COPIED_STIMULI={stimuli}")
PY

printf '=== VERIFY PRODUCTION PRESERVATION ===\n'
[ "$("$systemctl_command" --user is-active "$service_name")" = "active" ] || fail "production service is not active after proof"
production_image_after=$("$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://')
[ "$production_image_after" = "$production_image_before" ] || fail "production image changed during proof"
production_network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$production_network_after" = "$production_network_before" ] || fail "production network changed during proof"
provider_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$provider_after" = "$provider_before" ] || fail "production provider total changed during proof"
production_cursor_after=$(sqlite3 -readonly "$cycle_sidecar" "$cursor_query")
[ "$production_cursor_after" = "$production_cursor_before" ] || fail "production autonomous cycle changed during proof"
production_cycle_tasks_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
[ "$production_cycle_tasks_after" = "$production_cycle_tasks_before" ] || fail "production Local Goose cycle Task set changed"
production_dialogue_tasks_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';")
[ "$production_dialogue_tasks_after" = "$production_dialogue_tasks_before" ] || fail "isolated proof created a production dialogue Task"
production_stimuli_after=$(sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")
[ "$production_stimuli_after" = "$production_stimuli_before" ] || fail "production stimulus ledger changed"

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'MODEL_ID=%s\n' "$model_id"
printf 'MODEL_SHA256=%s\n' "$model_sha256"
printf 'PROOF_NETWORK=%s\n' "$proof_network"
printf 'PRODUCTION_IMAGE=%s\n' "$production_image_after"
printf 'PRODUCTION_NETWORK=%s\n' "$production_network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'PRODUCTION_CURSOR=%s\n' "$production_cursor_after"
printf 'PRODUCTION_DIALOGUE_TASKS=%s\n' "$production_dialogue_tasks_after"
printf 'PRODUCTION_STIMULI=%s\n' "$production_stimuli_after"
printf 'FEDORA_LOCAL_GOOSE_DIALOGUE_ISOLATED=PASS\n'
proof_ok=1
