#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_production_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-b516b78bd878ad5ee18abfe1d5fbb514b4d8520e99a904d90c899d3d2b7a8d1c}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
root_task_id=cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19
expected_head_task_id=cognition.local-goose-dialogue.v3:b5ff22b1696e7fd7a84005fd292a3b69d4d5ffd936cdb6507af1cb220e201813
thread_alias=main
manual_consumer_id=sol
responder_consumer_id=system-responder-v1
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
thread_sidecar="$state_directory/local-goose-dialogue-thread.db"
completion_sidecar="$state_directory/local-goose-dialogue-completion.db"
responder_sidecar="$state_directory/local-goose-dialogue-responder.db"
model_registry="$goose_root/data/models/registry.json"
proof_root="$data_home/gaudere/.gaudere-chat-proof"
build_script="$script_directory/build-image.sh"
provenance_script="$script_directory/verify-image-provenance.sh"

workspace=""
proof_image=""
phase_container=""
fake_pids=""
proof_ok=0

fail()
{
    printf 'gaudere-chat Fedora proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ -n "$phase_container" ]; then
        "$podman_command" rm -f "$phase_container" >/dev/null 2>&1 || true
    fi
    for pid in $fake_pids; do
        kill "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    done
    if [ "$proof_ok" = "1" ] && [ -n "$proof_image" ]; then
        "$podman_command" image rm "$proof_image" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'gaudere-chat Fedora proof: diagnostic directory preserved: %s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sha256sum tar cp mktemp sed awk grep dirname rm mkdir id sleep stat cut kill; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" "$model_registry" "$build_script" "$provenance_script"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production/proof file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
short_ref=$(printf '%s\n' "$agent_ref" | cut -c1-12)
host_uid=$(id -u)
host_gid=$(id -g)
proof_image="localhost/gaudere-agent:gaudere-chat-proof-$short_ref"

running_image()
{
    "$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://'
}

provider_total()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';"
}

cycle_cursor()
{
    sqlite3 -readonly "$cycle_sidecar" "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';"
}

task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='$1';"
}

stimulus_count()
{
    sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;'
}

verify_production_state()
{
    [ "$("$systemctl_command" --user is-active "$service_name")" = "active" ] || fail "production service is not active"
    [ "$(running_image)" = "$expected_production_image" ] || fail "production image differs from expected Stage 9J6 image"
    [ "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" = "none" ] || fail "production network is not none"
    [ "$(provider_total)" = "$expected_provider_total" ] || fail "production provider total differs"
    [ "$(cycle_cursor)" = "$expected_cycle_cursor" ] || fail "production autonomous cycle cursor differs"
    [ "$(task_count cognition.local-goose-cycle.v1)" = "1" ] || fail "production cycle Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v1)" = "1" ] || fail "production V1 Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v2)" = "2" ] || fail "production V2 Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v3)" = "2" ] || fail "production V3 Task count differs"
    [ "$(stimulus_count)" = "0" ] || fail "production stimulus ledger differs"

    [ "$(stat -c '%a' "$thread_sidecar")" = "600" ] || fail "production thread sidecar mode differs"
    [ "$(stat -c '%a' "$completion_sidecar")" = "600" ] || fail "production completion sidecar mode differs"
    [ "$(stat -c '%a' "$responder_sidecar")" = "600" ] || fail "production responder sidecar mode differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "thread sidecar schema differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "completion sidecar schema differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=2 AND root_task_id='$root_task_id' AND head_task_id='$expected_head_task_id';")" = "1" ] || fail "production preferred head differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_history;')" = "3" ] || fail "production preferred history count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')" = "3" ] || fail "production completion event count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")" = "3" ] || fail "production completion materialization differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')" = "2" ] || fail "production completion consumer count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")" = "1" ] || fail "production sol cursor differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "2" ] || fail "production responder cursor differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=3 AND task_id='$expected_head_task_id';")" = "1" ] || fail "production pending sequence 3 differs"
}

printf '=== VERIFY PRODUCTION PRECONDITIONS ===\n'
verify_production_state
production_image_before=$(running_image)
provider_before=$(provider_total)
cursor_before=$(cycle_cursor)
stimuli_before=$(stimulus_count)

mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/run.XXXXXX")
runtime_dir="$workspace/runtime"
goose_copy="$workspace/goose"
fake_dir="$workspace/fake"
mkdir -p "$runtime_dir" "$goose_copy" "$fake_dir"

python3 - "$runtime_dir" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

root = Path(sys.argv[1])
sources = [Path(value) for value in sys.argv[2:]]

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

for source in sources:
    backup(source, root / source.name)
PY

[ "$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")" = "2" ] || fail "copied V3 Task count differs"
[ "$(sqlite3 -readonly "$runtime_dir/local-goose-dialogue-thread.db" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "2" ] || fail "copied preferred revision differs"
[ "$(sqlite3 -readonly "$runtime_dir/local-goose-dialogue-completion.db" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')" = "3" ] || fail "copied completion events differ"

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
    raise SystemExit("gaudere-chat proof requires the existing single-file GGUF registration")
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

printf '=== BUILD GAUDERE-CHAT PROOF CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$proof_image" sh "$build_script"
PODMAN="$podman_command" sh "$provenance_script" "$proof_image" "$agent_ref" "$core_ref"
"$podman_command" run --rm --network none --entrypoint /usr/bin/test "$proof_image" -x /usr/local/bin/gaudere-chat || fail "candidate lacks gaudere-chat"
"$podman_command" run --rm --network none --entrypoint /usr/bin/test "$proof_image" -x /usr/local/bin/gaudere-local-goose-dialogue-v3-proof || fail "candidate lacks V3 proof runtime"

start_phase()
{
    phase=$1
    phase_container="gaudere-chat-proof-$phase-$$"
    rm -f "$runtime_dir/control.sock"
    "$podman_command" run -d \
        --name "$phase_container" \
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
        --entrypoint /usr/local/bin/gaudere-local-goose-dialogue-v3-proof \
        "$proof_image" \
        --state /work/state.db \
        --model "$model_selector" \
        --model-sha256 "$model_sha256" \
        --control-socket /work/control.sock \
        --thread-sidecar /work/local-goose-dialogue-thread.db \
        --completion-sidecar /work/local-goose-dialogue-completion.db \
        --thread-alias "$thread_alias" \
        --expected-tasks 1 \
        --linger-ms 5000 >/dev/null

    [ "$("$podman_command" inspect "$phase_container" --format '{{.HostConfig.NetworkMode}}')" = "none" ] || fail "$phase proof container network is not none"
    attempt=0
    while [ "$attempt" -lt 200 ] && [ ! -S "$runtime_dir/control.sock" ]; do
        attempt=$((attempt + 1))
        sleep 0.1
    done
    [ -S "$runtime_dir/control.sock" ] || {
        "$podman_command" logs "$phase_container" >&2 || true
        fail "$phase control socket did not become ready"
    }
}

run_real_chat()
{
    message=$1
    output=$(printf '%s\n/quit\n' "$message" | "$podman_command" exec -i \
        --user "$host_uid:$host_gid" \
        "$phase_container" \
        /usr/local/bin/gaudere-chat \
        --socket /work/control.sock \
        --thread "$thread_alias" \
        --poll-ms 50 \
        --timeout-ms 120000 \
        --io-timeout-ms 60000 \
        --verbose 2>&1) || {
            "$podman_command" logs "$phase_container" >&2 || true
            fail "gaudere-chat real turn failed"
        }
    printf '%s\n' "$output" >&2
    printf '%s\n' "$output" | grep -q 'Gaudere> ' || fail "gaudere-chat did not print a Gaudere response"
    printf '%s\n' "$output" | grep -q 'gaudere-chat: request=chat-v1-' || fail "gaudere-chat verbose request evidence missing"
    printf '%s\n' "$output"
}

finish_phase()
{
    phase=$1
    container_exit=$("$podman_command" wait "$phase_container")
    logs=$("$podman_command" logs "$phase_container")
    printf '%s\n' "$logs"
    [ "$container_exit" = "0" ] || fail "$phase proof runtime exited with status $container_exit"
    printf '%s\n' "$logs" | grep -q '^FEDORA_LOCAL_GOOSE_DIALOGUE_V3_RUNTIME=PASS$' || fail "$phase runtime did not report PASS"
    printf '%s\n' "$logs" | grep -q 'provider_execution=false tools_enabled=false network_expected=none multi_actor=true preferred_head_cas=true completion_feed=true expected_tasks=1 linger_ms=5000' || fail "$phase authority/linger marker missing"
    "$podman_command" rm "$phase_container" >/dev/null
    phase_container=""
}

message_one="Bonjour Gaudere. Ceci est le premier tour de preuve isolée de gaudere-chat. Confirme brièvement que tu identifies Bertrand comme interlocuteur humain."
message_two="Deuxième tour de preuve gaudere-chat après redémarrage. Confirme brièvement que la conversation continue."

printf '=== REAL CHAT PHASE 1: HUMAN TURN ON PRODUCTION COPY ===\n'
start_phase phase1
chat_one=$(run_real_chat "$message_one")
finish_phase phase1
[ "$(sqlite3 -readonly "$runtime_dir/local-goose-dialogue-thread.db" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "3" ] || fail "phase 1 preferred revision is not 3"
[ "$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")" = "3" ] || fail "phase 1 V3 Task count is not 3"

printf '=== REAL CHAT PHASE 2: RESTART + SECOND HUMAN TURN ===\n'
start_phase phase2
chat_two=$(run_real_chat "$message_two")
finish_phase phase2
[ "$(sqlite3 -readonly "$runtime_dir/local-goose-dialogue-thread.db" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "4" ] || fail "phase 2 preferred revision is not 4"
[ "$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")" = "4" ] || fail "phase 2 V3 Task count is not 4"

printf '=== VERIFY REAL CHAT CANONICAL EVIDENCE ===\n'
python3 - \
    "$runtime_dir/state.db" \
    "$runtime_dir/local-goose-cycle.db" \
    "$runtime_dir/local-goose-cycle-stimulus.db" \
    "$runtime_dir/local-goose-dialogue-thread.db" \
    "$runtime_dir/local-goose-dialogue-completion.db" \
    "$root_task_id" "$expected_head_task_id" \
    "$message_one" "$message_two" "$model_sha256" \
    "$expected_cycle_cursor" "$expected_provider_total" <<'PY'
import hashlib
import json
from pathlib import Path
import sqlite3
import sys

(
    state_path,
    cycle_path,
    stimulus_path,
    thread_path,
    completion_path,
    root_id,
    initial_head,
    message_one,
    message_two,
    model_sha,
    expected_cursor,
    expected_provider,
) = sys.argv[1:]
expected_provider = int(expected_provider)

thread = sqlite3.connect(Path(thread_path))
head = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_head WHERE alias='main'"
).fetchone()
history = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_history WHERE alias='main' ORDER BY revision"
).fetchall()
thread.close()
if head is None or head[0] != 4 or head[1] != root_id:
    raise SystemExit(f"final preferred head differs: {head!r}")
if len(history) != 5 or [row[0] for row in history] != [0, 1, 2, 3, 4]:
    raise SystemExit(f"preferred history differs: {history!r}")
if history[2][2] != initial_head:
    raise SystemExit("copied production revision 2 head differs")

first_id = history[3][2]
second_id = history[4][2]
state = sqlite3.connect(Path(state_path))
provider = state.execute(
    "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses'"
).fetchone()[0]
if provider != expected_provider:
    raise SystemExit(f"copied provider total changed: {provider}")
v3_count = state.execute(
    "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3'"
).fetchone()[0]
if v3_count != 4:
    raise SystemExit(f"expected four V3 Tasks on copy, got {v3_count}")

predecessors = [initial_head, first_id]
messages = [message_one, message_two]
turns = [4, 5]
responses = []
for task_id, predecessor_id, message, turn_index in zip(
    (first_id, second_id), predecessors, messages, turns
):
    predecessor = state.execute(
        "SELECT result_output FROM tasks WHERE id=? AND status=3",
        (predecessor_id,),
    ).fetchone()
    task = state.execute(
        "SELECT input,status,attempts_started,result_content_type,result_output,"
        "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
        "FROM tasks WHERE id=? AND kind='cognition.local-goose-dialogue.v3'",
        (task_id,),
    ).fetchone()
    if predecessor is None or task is None:
        raise SystemExit("chat lineage Task is missing")
    raw_input, status, attempts, result_type, raw_result, failure_code, failure_message = task
    if status != 3 or attempts != 1 or failure_code or failure_message:
        raise SystemExit(f"chat Task is not one canonical success: {task_id}")
    if result_type != "application/vnd.gaudere.local-goose-dialogue-v3-response+json":
        raise SystemExit("chat result content type differs")
    inp = json.loads(raw_input)
    predecessor_sha = hashlib.sha256(predecessor[0].encode("utf-8")).hexdigest()
    if (
        inp.get("schema") != "gaudere.cognition.local-goose-dialogue.v3"
        or not str(inp.get("request_id", "")).startswith("chat-v1-")
        or inp.get("speaker_kind") != "human"
        or inp.get("speaker_id") != "bertrand"
        or inp.get("message_kind") != "dialogue"
        or inp.get("message") != message
        or inp.get("model_sha256") != model_sha
        or inp.get("turn_index") != turn_index
        or inp.get("root_task_id") != root_id
        or inp.get("predecessor_task_id") != predecessor_id
        or inp.get("predecessor_result_sha256") != predecessor_sha
    ):
        raise SystemExit(f"chat canonical input differs: {task_id}")
    expected_id = "cognition.local-goose-dialogue.v3:" + hashlib.sha256(
        raw_input.encode("utf-8")
    ).hexdigest()
    if task_id != expected_id:
        raise SystemExit("chat Task id differs from canonical input hash")

    out = json.loads(raw_result)
    if (
        out.get("schema") != "gaudere.cognition.local-goose-dialogue-v3-response.v1"
        or out.get("request_id") != inp["request_id"]
        or out.get("speaker_kind") != "human"
        or out.get("speaker_id") != "bertrand"
        or out.get("message_kind") != "dialogue"
        or out.get("model_sha256") != model_sha
        or out.get("turn_index") != turn_index
        or out.get("root_task_id") != root_id
        or out.get("predecessor_task_id") != predecessor_id
        or out.get("predecessor_result_sha256") != predecessor_sha
        or not isinstance(out.get("response"), str)
        or not out["response"].strip()
    ):
        raise SystemExit(f"chat canonical result differs: {task_id}")
    responses.append(out["response"])
state.close()

completion = sqlite3.connect(Path(completion_path))
events = completion.execute(
    "SELECT sequence,thread_revision,task_id FROM local_goose_dialogue_completion_event "
    "ORDER BY sequence"
).fetchall()
materialization = completion.execute(
    "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='main'"
).fetchone()
consumers = dict(completion.execute(
    "SELECT consumer_id,last_sequence FROM local_goose_dialogue_consumer_cursor"
).fetchall())
completion.close()
if len(events) != 5 or materialization != (5,):
    raise SystemExit("chat completion materialization differs")
if events[-2] != (4, 3, first_id) or events[-1] != (5, 4, second_id):
    raise SystemExit("chat completion event linkage differs")
if consumers != {"sol": 1, "system-responder-v1": 2}:
    raise SystemExit(f"chat mutated completion consumers: {consumers!r}")

cycle = sqlite3.connect(Path(cycle_path))
cursor = cycle.execute(
    "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||"
    "COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||"
    "COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') "
    "FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1'"
).fetchone()
cycle.close()
if cursor is None or cursor[0] != expected_cursor:
    raise SystemExit("copied autonomous cycle changed during chat proof")

stimulus = sqlite3.connect(Path(stimulus_path))
stimuli = stimulus.execute("SELECT COUNT(*) FROM local_goose_cycle_stimuli").fetchone()[0]
stimulus.close()
if stimuli != 0:
    raise SystemExit("copied stimulus ledger changed during chat proof")

print(f"CHAT_TASK_1={first_id}")
print(f"CHAT_TASK_2={second_id}")
print("CHAT_RESPONSE_1=" + json.dumps(responses[0], ensure_ascii=False))
print("CHAT_RESPONSE_2=" + json.dumps(responses[1], ensure_ascii=False))
print("CHAT_FINAL_REVISION=4")
print("CHAT_COMPLETION_EVENTS=5")
print("CHAT_CONSUMERS=sol:1,system-responder-v1:2")
print("CHAT_RESPONDER_NEXT_PENDING_SEQUENCE=3")
PY

printf '=== BINARY CONFLICT PROOF: NO SILENT REBASE/RESUBMIT ===\n'
"$podman_command" run --rm --network none --userns=keep-id --user "$host_uid:$host_gid" --volume "$fake_dir:/work:Z" --entrypoint /usr/bin/true "$proof_image"

conflict_socket="$fake_dir/conflict.sock"
conflict_log="$fake_dir/conflict.log"
conflict_message="Conflit CAS volontaire: ce message ne doit pas être automatiquement rebasé."
python3 - "$conflict_socket" "$root_task_id" "$expected_head_task_id" "$conflict_message" >"$conflict_log" 2>&1 <<'PY' &
import json
import os
import socket
import sys

path, root_id, initial_head, expected_message = sys.argv[1:]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
os.chmod(path, 0o600)
server.listen(4)
new_head = "cognition.local-goose-dialogue.v3:" + "a" * 64

def receive(conn):
    data = bytearray()
    while True:
        block = conn.recv(4096)
        if not block:
            break
        data.extend(block)
    return json.loads(data.decode("utf-8"))

def reply(conn, ok, code, body):
    payload = json.dumps(
        {"version": 1, "ok": ok, "code": code, "body": body},
        separators=(",", ":"),
    ).encode("utf-8")
    conn.sendall(payload)

for index in range(3):
    conn, _ = server.accept()
    with conn:
        request = receive(conn)
        if request.get("version") != 1:
            raise SystemExit("unexpected protocol version")
        if index == 0:
            if request.get("operation") != "inspect_local_goose_dialogue_thread_head":
                raise SystemExit("first command is not head inspection")
            reply(
                conn, True, 0,
                f'alias="main"\nrevision=2\nroot_task_id="{root_id}"\n'
                f'head_task_id="{initial_head}"\n',
            )
        elif index == 1:
            if (
                request.get("operation")
                != "submit_local_goose_dialogue_v3_preferred_next"
                or request.get("expected_thread_revision") != 2
                or request.get("speaker_kind") != "human"
                or request.get("speaker_id") != "bertrand"
                or request.get("message_kind") != "dialogue"
                or request.get("text") != expected_message
            ):
                raise SystemExit(f"unexpected preferred submit: {request!r}")
            reply(
                conn, False, 4,
                "gaudere-agent: preferred dialogue thread revision conflict\n"
                f'alias="main"\nrevision=3\nroot_task_id="{root_id}"\n'
                f'head_task_id="{new_head}"\n',
            )
        else:
            if request.get("operation") != "inspect_local_goose_dialogue_thread_head":
                raise SystemExit("third command is not refreshed head inspection")
            reply(
                conn, True, 0,
                f'alias="main"\nrevision=3\nroot_task_id="{root_id}"\n'
                f'head_task_id="{new_head}"\n',
            )
server.close()
print("GAUDERE_CHAT_FAKE_CONFLICT=PASS")
PY
conflict_pid=$!
fake_pids="$fake_pids $conflict_pid"
attempt=0
while [ "$attempt" -lt 100 ] && [ ! -S "$conflict_socket" ]; do
    attempt=$((attempt + 1))
    sleep 0.05
done
[ -S "$conflict_socket" ] || fail "fake conflict socket did not become ready"

conflict_output=$(printf '%s\n/quit\n' "$conflict_message" | "$podman_command" run --rm -i \
    --network none \
    --userns=keep-id \
    --user "$host_uid:$host_gid" \
    --volume "$fake_dir:/work:Z" \
    --entrypoint /usr/local/bin/gaudere-chat \
    "$proof_image" \
    --socket /work/conflict.sock \
    --thread main \
    --io-timeout-ms 2000 \
    --timeout-ms 5000 \
    --verbose 2>&1) || fail "gaudere-chat conflict binary proof failed"
printf '%s\n' "$conflict_output"
wait "$conflict_pid" || {
    cat "$conflict_log" >&2 || true
    fail "fake conflict server failed"
}
fake_pids=$(printf '%s\n' "$fake_pids" | sed "s/ $conflict_pid//")
grep -q '^GAUDERE_CHAT_FAKE_CONFLICT=PASS$' "$conflict_log" || fail "fake conflict server did not observe exact three-command flow"
printf '%s\n' "$conflict_output" | grep -q 'preferred thread advanced concurrently' || fail "gaudere-chat did not report explicit CAS conflict"
! printf '%s\n' "$conflict_output" | grep -q 'Gaudere> ' || fail "conflicted message unexpectedly produced a Gaudere response"

printf '=== BINARY I/O TIMEOUT PROOF ===\n'
timeout_socket="$fake_dir/timeout.sock"
timeout_log="$fake_dir/timeout.log"
python3 - "$timeout_socket" >"$timeout_log" 2>&1 <<'PY' &
import json
import os
import socket
import sys
import time

path = sys.argv[1]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
os.chmod(path, 0o600)
server.listen(1)
conn, _ = server.accept()
with conn:
    data = bytearray()
    while True:
        block = conn.recv(4096)
        if not block:
            break
        data.extend(block)
    request = json.loads(data.decode("utf-8"))
    if request.get("operation") != "inspect_local_goose_dialogue_thread_head":
        raise SystemExit(f"unexpected timeout command: {request!r}")
    time.sleep(1.0)
server.close()
print("GAUDERE_CHAT_FAKE_TIMEOUT=PASS")
PY
timeout_pid=$!
fake_pids="$fake_pids $timeout_pid"
attempt=0
while [ "$attempt" -lt 100 ] && [ ! -S "$timeout_socket" ]; do
    attempt=$((attempt + 1))
    sleep 0.05
done
[ -S "$timeout_socket" ] || fail "fake timeout socket did not become ready"

timeout_output=$(printf '/head\n/quit\n' | "$podman_command" run --rm -i \
    --network none \
    --userns=keep-id \
    --user "$host_uid:$host_gid" \
    --volume "$fake_dir:/work:Z" \
    --entrypoint /usr/local/bin/gaudere-chat \
    "$proof_image" \
    --socket /work/timeout.sock \
    --thread main \
    --io-timeout-ms 100 \
    --timeout-ms 5000 2>&1) || fail "gaudere-chat timeout binary proof process failed"
printf '%s\n' "$timeout_output"
printf '%s\n' "$timeout_output" | grep -q 'live control receive timed out' || fail "gaudere-chat did not surface bounded live-control I/O timeout"
wait "$timeout_pid" || {
    cat "$timeout_log" >&2 || true
    fail "fake timeout server failed"
}
fake_pids=$(printf '%s\n' "$fake_pids" | sed "s/ $timeout_pid//")
grep -q '^GAUDERE_CHAT_FAKE_TIMEOUT=PASS$' "$timeout_log" || fail "fake timeout server did not complete expected stall"

printf '=== VERIFY PRODUCTION PRESERVATION ===\n'
verify_production_state
[ "$(running_image)" = "$production_image_before" ] || fail "production image changed during isolated chat proof"
[ "$(provider_total)" = "$provider_before" ] || fail "production provider total changed during isolated chat proof"
[ "$(cycle_cursor)" = "$cursor_before" ] || fail "production autonomous cycle changed during isolated chat proof"
[ "$(stimulus_count)" = "$stimuli_before" ] || fail "production stimulus ledger changed during isolated chat proof"

proof_image_id=$("$podman_command" image inspect --format '{{.Id}}' "$proof_image" | sed 's/^sha256://')
printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PROOF_IMAGE=%s\n' "$proof_image_id"
printf 'PROOF_NETWORK=none\n'
printf 'PRODUCTION_IMAGE=%s\n' "$(running_image)"
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'CYCLE_CURSOR=%s\n' "$(cycle_cursor)"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$(task_count cognition.local-goose-cycle.v1)"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v1)"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v2)"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v3)"
printf 'STIMULI=%s\n' "$(stimulus_count)"
printf 'DIALOGUE_THREAD_REVISION=2\n'
printf 'DIALOGUE_THREAD_HEAD=%s\n' "$expected_head_task_id"
printf 'DIALOGUE_COMPLETION_EVENTS=3\n'
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=3\n'
printf 'DIALOGUE_COMPLETION_CONSUMERS=2\n'
printf 'DIALOGUE_CONSUMER_SOL=sol:1\n'
printf 'DIALOGUE_CONSUMER_RESPONDER=system-responder-v1:2\n'
printf 'DIALOGUE_RESPONDER_NEXT_PENDING_SEQUENCE=3\n'
printf 'FEDORA_GAUDERE_CHAT_ISOLATED=PASS\n'
proof_ok=1
