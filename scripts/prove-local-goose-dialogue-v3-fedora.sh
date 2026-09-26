#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_production_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-95660bd47c52de5bbac298047055f6b390018c6f86bee7457e59a88d18f0e3b6}
expected_root_task_id=${GAUDERE_EXPECTED_DIALOGUE_V2_ROOT_TASK_ID:-cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19}
expected_v2_head_task_id=${GAUDERE_EXPECTED_DIALOGUE_V2_HEAD_TASK_ID:-cognition.local-goose-dialogue.v2:3723223592d31a36116fd30da83a49c23519347313e1fb1d90127dbd4d0da11d}
expected_v2_head_result_sha256=${GAUDERE_EXPECTED_DIALOGUE_V2_HEAD_RESULT_SHA256:-0bbd1857634f2c6502d8700c3e7819f48bce071d7c1c4267146d1308469e88a0}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
model_selector=/$model_id
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
thread_alias=${GAUDERE_DIALOGUE_THREAD_ALIAS:-main}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
goose_root=${GAUDERE_GOOSE_ROOT:-"$data_home/gaudere/goose"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
model_registry="$goose_root/data/models/registry.json"
proof_root="$data_home/gaudere/.local-goose-dialogue-v3-proof"
workspace=""
proof_image=""
container_prefix="gaudere-local-goose-dialogue-v3-proof-$$"
container_name=""
proof_ok=0

fail()
{
    printf 'gaudere Local Goose dialogue v3 Fedora proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    for name in "$container_prefix-phase1" "$container_prefix-phase2"; do
        "$podman_command" rm -f "$name" >/dev/null 2>&1 || true
    done
    if [ "$proof_ok" = "1" ] && [ -n "$proof_image" ]; then
        "$podman_command" image rm "$proof_image" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'gaudere Local Goose dialogue v3 Fedora proof: diagnostic directory preserved: %s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sha256sum tar cp mktemp sed awk grep dirname rm mkdir id sleep cut stat; do
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
proof_image="localhost/gaudere-agent:dialogue-v3-proof-$short_ref"

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
production_v1_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';")
production_v2_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v2';")
production_v3_before=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")
production_stimuli_before=$(sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")
[ "$production_cycle_tasks_before" = "1" ] || fail "production must contain exactly one Local Goose cycle Task"
[ "$production_v1_before" = "1" ] || fail "production must contain exactly one proven V1 dialogue Task"
[ "$production_v2_before" = "2" ] || fail "production must contain exactly two proven V2 dialogue Tasks"
[ "$production_v3_before" = "0" ] || fail "production already contains V3 dialogue Tasks"
[ "$production_stimuli_before" = "0" ] || fail "production stimulus ledger is not empty"

python3 - "$state_database" "$expected_root_task_id" "$expected_v2_head_task_id" "$expected_v2_head_result_sha256" "$model_sha256" <<'PY'
import hashlib
import json
import sqlite3
import sys

db, root_id, head_id, expected_head_sha, model_sha = sys.argv[1:]
con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
rows = {}
for task_id in (root_id, head_id):
    row = con.execute(
        "SELECT kind,input,status,attempts_started,result_content_type,result_output,"
        "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
        "FROM tasks WHERE id=?",
        (task_id,),
    ).fetchone()
    if row is None:
        raise SystemExit(f"missing production V2 Task {task_id}")
    rows[task_id] = row
con.close()

for task_id, row in rows.items():
    kind, raw_input, status, attempts, content_type, raw_result, failure_code, failure_message = row
    if kind != "cognition.local-goose-dialogue.v2" or status != 3 or attempts != 1:
        raise SystemExit(f"production V2 Task is not canonical single success: {task_id}")
    if content_type != "application/vnd.gaudere.local-goose-dialogue-v2-response+json":
        raise SystemExit(f"production V2 result content type differs: {task_id}")
    if failure_code or failure_message:
        raise SystemExit(f"production V2 Task contains failure evidence: {task_id}")
    request = json.loads(raw_input)
    result = json.loads(raw_result)
    if request.get("model_sha256") != model_sha or result.get("model_sha256") != model_sha:
        raise SystemExit(f"production V2 model identity differs: {task_id}")

head_raw = rows[head_id][5]
actual = hashlib.sha256(head_raw.encode("utf-8")).hexdigest()
if actual != expected_head_sha:
    raise SystemExit(f"production V2 head result SHA differs: {actual}")
head_input = json.loads(rows[head_id][1])
head_result = json.loads(head_raw)
if (
    head_input.get("turn_index") != 1
    or head_input.get("root_task_id") != root_id
    or head_input.get("predecessor_task_id") != root_id
    or head_result.get("turn_index") != 1
    or head_result.get("root_task_id") != root_id
):
    raise SystemExit("production V2 head lineage differs")
PY

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
copy_provider_before=$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
copy_stimuli_before=$(sqlite3 -readonly "$runtime_dir/local-goose-cycle-stimulus.db" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")
copy_v3_before=$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")
[ "$copy_cursor_before" = "$production_cursor_before" ] || fail "copied cycle cursor differs from production"
[ "$copy_provider_before" = "$provider_before" ] || fail "copied provider total differs"
[ "$copy_stimuli_before" = "$production_stimuli_before" ] || fail "copied stimulus ledger differs"
[ "$copy_v3_before" = "0" ] || fail "copied state unexpectedly contains V3 dialogue Tasks"

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
    raise SystemExit("dialogue v3 proof requires the existing single-file GGUF registration")
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

printf '=== BUILD DIALOGUE V3 PROOF CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$proof_image" sh "$script_directory/build-image.sh"

"$podman_command" run --rm --network none --entrypoint /usr/bin/test "$proof_image" -x /usr/local/bin/gaudere-local-goose-dialogue-v3-proof || fail "candidate lacks dialogue v3 proof runtime"

start_phase()
{
    phase=$1
    expected_tasks=$2
    container_name="$container_prefix-$phase"
    rm -f "$runtime_dir/control.sock"
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
        --entrypoint /usr/local/bin/gaudere-local-goose-dialogue-v3-proof \
        "$proof_image" \
        --state /work/state.db \
        --model "$model_selector" \
        --model-sha256 "$model_sha256" \
        --control-socket /work/control.sock \
        --thread-sidecar /work/local-goose-dialogue-thread.db \
        --completion-sidecar /work/local-goose-dialogue-completion.db \
        --thread-alias "$thread_alias" \
        --expected-tasks "$expected_tasks" >/dev/null

    [ "$("$podman_command" inspect "$container_name" --format '{{.HostConfig.NetworkMode}}')" = "none" ] || fail "$phase proof container network is not none"

    attempt=0
    while [ "$attempt" -lt 200 ] && [ ! -S "$runtime_dir/control.sock" ]; do
        attempt=$((attempt + 1))
        sleep 0.1
    done
    [ -S "$runtime_dir/control.sock" ] || {
        "$podman_command" logs "$container_name" >&2 || true
        fail "$phase dialogue v3 proof control socket did not become ready"
    }
}

control()
{
    "$podman_command" exec --user "$host_uid:$host_gid" "$container_name" \
        /usr/local/bin/gaudere-control --socket /work/control.sock "$@"
}

submit_preferred()
{
    request_id=$1
    revision=$2
    speaker_kind=$3
    speaker_id=$4
    message_kind=$5
    message=$6
    output=$(control local-thread-v3-send \
        "$request_id" "$thread_alias" "$revision" \
        "$speaker_kind" "$speaker_id" "$message_kind" "$message") || return 1
    printf '%s\n' "$output" >&2
    printf '%s\n' "$output" | grep -q '^kind="cognition.local-goose-dialogue.v3"$' || return 1
    printf '%s\n' "$output" | sed -n 's/^id="\(.*\)"$/\1/p' | head -n 1
}

wait_success()
{
    task_id=$1
    attempt=0
    status=""
    while [ "$attempt" -lt 300 ]; do
        attempt=$((attempt + 1))
        status=$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT status FROM tasks WHERE id='$task_id';")
        [ "$status" = "3" ] && return 0
        case "$status" in
            4|5|6) return 1 ;;
        esac
        sleep 1
    done
    return 1
}

finish_phase()
{
    phase=$1
    expected_tasks=$2
    container_exit=$("$podman_command" wait "$container_name")
    logs=$("$podman_command" logs "$container_name")
    printf '%s\n' "$logs"
    [ "$container_exit" = "0" ] || fail "$phase dialogue v3 proof runtime exited with status $container_exit"
    printf '%s\n' "$logs" | grep -q '^FEDORA_LOCAL_GOOSE_DIALOGUE_V3_RUNTIME=PASS$' || fail "$phase runtime did not report PASS"
    printf '%s\n' "$logs" | grep -q "provider_execution=false tools_enabled=false network_expected=none multi_actor=true preferred_head_cas=true completion_feed=true expected_tasks=$expected_tasks" || fail "$phase authority marker missing"
    "$podman_command" rm "$container_name" >/dev/null
    container_name=""
}

human_request="fedora-v3-human-$short_ref"
observation_request="fedora-v3-system-observation-$short_ref"
feedback_request="fedora-v3-system-feedback-$short_ref"
stale_request="fedora-v3-stale-conflict-$short_ref"

human_message="Bertrand reprend ce fil en V3. Confirme brièvement que ce message vient de Bertrand, un humain."
observation_message="Observation système: la preuve Fedora V3 est en cours et le tour humain précédent a réussi. Distingue explicitement cette observation système d'un message de Bertrand."
feedback_message="Retour système: conserve la distinction entre Bertrand et le système. Résume brièvement qui t'a parlé lors des deux tours précédents."
stale_message="Ce message ne doit jamais créer de Task car sa révision de tête est obsolète."

printf '=== PHASE 1: BIND REAL V2 PRODUCTION COPY + HUMAN V3 TURN ===\n'
start_phase phase1 1
bind_output=$(control local-thread-v3-bind "$thread_alias" "$expected_v2_head_task_id") || fail "V2 production-copy head bind failed"
printf '%s\n' "$bind_output"
printf '%s\n' "$bind_output" | grep -q "^alias=\"$thread_alias\"$" || fail "bound alias differs"
printf '%s\n' "$bind_output" | grep -q '^revision=0$' || fail "initial preferred revision is not zero"
printf '%s\n' "$bind_output" | grep -q "^root_task_id=\"$expected_root_task_id\"$" || fail "bound root differs"
printf '%s\n' "$bind_output" | grep -q "^head_task_id=\"$expected_v2_head_task_id\"$" || fail "bound V2 head differs"

human_task=$(submit_preferred "$human_request" 0 human bertrand dialogue "$human_message") || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "human V3 preferred submission failed"
}
[ -n "$human_task" ] || fail "could not resolve human V3 Task id"
wait_success "$human_task" || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "human V3 Task did not succeed"
}
finish_phase phase1 1

[ "$(stat -c '%a' "$runtime_dir/local-goose-dialogue-thread.db")" = "600" ] || fail "thread sidecar mode differs from 0600"
[ "$(stat -c '%a' "$runtime_dir/local-goose-dialogue-completion.db")" = "600" ] || fail "completion sidecar mode differs from 0600"
[ "$(sqlite3 -readonly "$runtime_dir/local-goose-dialogue-thread.db" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "1" ] || fail "phase 1 preferred revision is not one"
[ "$(sqlite3 -readonly "$runtime_dir/local-goose-dialogue-completion.db" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE thread_alias='$thread_alias';")" = "2" ] || fail "phase 1 completion feed does not contain baseline plus human turn"

printf '=== PHASE 2: RESTART + SYSTEM OBSERVATION + CAS CONFLICT + SYSTEM FEEDBACK ===\n'
start_phase phase2 2
head_output=$(control local-thread-v3-head "$thread_alias") || fail "preferred head inspection after restart failed"
printf '%s\n' "$head_output"
printf '%s\n' "$head_output" | grep -q '^revision=1$' || fail "preferred revision did not survive restart"
printf '%s\n' "$head_output" | grep -q "^head_task_id=\"$human_task\"$" || fail "preferred head did not survive restart"

observation_task=$(submit_preferred "$observation_request" 1 system gaudere-runtime observation "$observation_message") || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "system observation V3 submission failed"
}
[ -n "$observation_task" ] || fail "could not resolve system observation Task id"
wait_success "$observation_task" || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "system observation V3 Task did not succeed"
}

if stale_output=$(control local-thread-v3-send \
    "$stale_request" "$thread_alias" 1 system sol feedback "$stale_message" 2>&1); then
    printf '%s\n' "$stale_output" >&2
    fail "stale preferred revision unexpectedly succeeded"
fi
printf '%s\n' "$stale_output"
printf '%s\n' "$stale_output" | grep -q 'preferred dialogue thread revision conflict' || fail "stale revision did not report explicit CAS conflict"
[ "$(sqlite3 -readonly "$runtime_dir/state.db" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")" = "2" ] || fail "stale revision created an unexpected V3 Task"

feedback_task=$(submit_preferred "$feedback_request" 2 system sol feedback "$feedback_message") || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "system feedback V3 submission failed"
}
[ -n "$feedback_task" ] || fail "could not resolve system feedback Task id"
wait_success "$feedback_task" || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "system feedback V3 Task did not succeed"
}
finish_phase phase2 2

printf '=== VERIFY ISOLATED MULTI-ACTOR EVIDENCE ===\n'
python3 - \
    "$runtime_dir/state.db" \
    "$runtime_dir/local-goose-cycle.db" \
    "$runtime_dir/local-goose-cycle-stimulus.db" \
    "$runtime_dir/local-goose-dialogue-thread.db" \
    "$runtime_dir/local-goose-dialogue-completion.db" \
    "$expected_root_task_id" "$expected_v2_head_task_id" "$expected_v2_head_result_sha256" \
    "$human_task" "$observation_task" "$feedback_task" \
    "$human_request" "$observation_request" "$feedback_request" "$stale_request" \
    "$thread_alias" "$model_sha256" "$copy_cursor_before" "$copy_provider_before" "$copy_stimuli_before" <<'PY'
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
    feed_path,
    root_id,
    v2_head_id,
    v2_head_sha,
    human_id,
    observation_id,
    feedback_id,
    human_request,
    observation_request,
    feedback_request,
    stale_request,
    thread_alias,
    model_sha,
    expected_cursor,
    expected_provider,
    expected_stimuli,
) = sys.argv[1:]

expected_provider = int(expected_provider)
expected_stimuli = int(expected_stimuli)

state = sqlite3.connect(Path(state_path))
provider = state.execute(
    "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses'"
).fetchone()[0]
counts = {
    kind: state.execute("SELECT COUNT(*) FROM tasks WHERE kind=?", (kind,)).fetchone()[0]
    for kind in (
        "cognition.local-goose-cycle.v1",
        "cognition.local-goose-dialogue.v1",
        "cognition.local-goose-dialogue.v2",
        "cognition.local-goose-dialogue.v3",
    )
}

if provider != expected_provider:
    raise SystemExit(f"copied provider total changed to {provider}")
if counts["cognition.local-goose-cycle.v1"] != 1:
    raise SystemExit("copied Local Goose cycle Task count changed")
if counts["cognition.local-goose-dialogue.v1"] != 1:
    raise SystemExit("copied V1 dialogue Task count changed")
if counts["cognition.local-goose-dialogue.v2"] != 2:
    raise SystemExit("copied V2 dialogue Task count changed")
if counts["cognition.local-goose-dialogue.v3"] != 3:
    raise SystemExit(f"expected three V3 Tasks, got {counts['cognition.local-goose-dialogue.v3']}")

v2_head = state.execute(
    "SELECT result_output FROM tasks WHERE id=? AND status=3",
    (v2_head_id,),
).fetchone()
if v2_head is None:
    raise SystemExit("copied V2 head is missing")
actual_v2_head_sha = hashlib.sha256(v2_head[0].encode("utf-8")).hexdigest()
if actual_v2_head_sha != v2_head_sha:
    raise SystemExit("copied V2 head result SHA differs")

expected = [
    (
        human_id,
        human_request,
        "human",
        "bertrand",
        "dialogue",
        2,
        v2_head_id,
        v2_head_sha,
    ),
    (
        observation_id,
        observation_request,
        "system",
        "gaudere-runtime",
        "observation",
        3,
        human_id,
        None,
    ),
    (
        feedback_id,
        feedback_request,
        "system",
        "sol",
        "feedback",
        4,
        observation_id,
        None,
    ),
]

result_hashes = {v2_head_id: v2_head_sha}
responses = {}
for task_id, request_id, speaker_kind, speaker_id, message_kind, turn_index, predecessor_id, fixed_pred_sha in expected:
    row = state.execute(
        "SELECT input,status,attempts_started,result_content_type,result_output,"
        "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
        "FROM tasks WHERE id=?",
        (task_id,),
    ).fetchone()
    if row is None:
        raise SystemExit(f"missing V3 Task {task_id}")
    raw_input, status, attempts, content_type, raw_result, failure_code, failure_message = row
    if status != 3 or attempts != 1:
        raise SystemExit(f"{task_id} did not succeed exactly once")
    if content_type != "application/vnd.gaudere.local-goose-dialogue-v3-response+json":
        raise SystemExit(f"{task_id} result content type differs")
    if failure_code or failure_message:
        raise SystemExit(f"{task_id} contains failure evidence")

    request = json.loads(raw_input)
    result = json.loads(raw_result)
    predecessor_sha = fixed_pred_sha or result_hashes.get(predecessor_id)
    if predecessor_sha is None:
        raise SystemExit(f"missing predecessor result SHA for {task_id}")

    expected_input_keys = {
        "message",
        "message_kind",
        "model_sha256",
        "predecessor_result_sha256",
        "predecessor_task_id",
        "request_id",
        "root_task_id",
        "schema",
        "speaker_id",
        "speaker_kind",
        "turn_index",
    }
    if set(request) != expected_input_keys:
        raise SystemExit(f"{task_id} input keys differ")
    if (
        request.get("schema") != "gaudere.cognition.local-goose-dialogue.v3"
        or request.get("request_id") != request_id
        or request.get("speaker_kind") != speaker_kind
        or request.get("speaker_id") != speaker_id
        or request.get("message_kind") != message_kind
        or request.get("model_sha256") != model_sha
        or request.get("turn_index") != turn_index
        or request.get("root_task_id") != root_id
        or request.get("predecessor_task_id") != predecessor_id
        or request.get("predecessor_result_sha256") != predecessor_sha
    ):
        raise SystemExit(f"{task_id} canonical input evidence differs")

    expected_id = "cognition.local-goose-dialogue.v3:" + hashlib.sha256(
        raw_input.encode("utf-8")
    ).hexdigest()
    if task_id != expected_id:
        raise SystemExit(f"{task_id} identity differs from canonical input bytes")

    expected_result_keys = {
        "message_kind",
        "model_sha256",
        "predecessor_result_sha256",
        "predecessor_task_id",
        "request_id",
        "response",
        "root_task_id",
        "schema",
        "speaker_id",
        "speaker_kind",
        "turn_index",
    }
    if set(result) != expected_result_keys:
        raise SystemExit(f"{task_id} result keys differ")
    if (
        result.get("schema") != "gaudere.cognition.local-goose-dialogue-v3-response.v1"
        or result.get("request_id") != request_id
        or result.get("speaker_kind") != speaker_kind
        or result.get("speaker_id") != speaker_id
        or result.get("message_kind") != message_kind
        or result.get("model_sha256") != model_sha
        or result.get("turn_index") != turn_index
        or result.get("root_task_id") != root_id
        or result.get("predecessor_task_id") != predecessor_id
        or result.get("predecessor_result_sha256") != predecessor_sha
        or not isinstance(result.get("response"), str)
        or not result["response"].strip()
        or len(result["response"].encode("utf-8")) > 16 * 1024
    ):
        raise SystemExit(f"{task_id} canonical result evidence differs")

    result_hashes[task_id] = hashlib.sha256(raw_result.encode("utf-8")).hexdigest()
    responses[task_id] = result["response"]

stale = state.execute(
    "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3' "
    "AND json_extract(input,'$.request_id')=?",
    (stale_request,),
).fetchone()[0]
state.close()
if stale != 0:
    raise SystemExit("stale CAS request unexpectedly created a durable Task")

cycle = sqlite3.connect(Path(cycle_path))
cursor = cycle.execute(
    "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'"
    "||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'"
    "||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') "
    "FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1'"
).fetchone()[0]
cycle.close()
if cursor != expected_cursor:
    raise SystemExit("copied autonomous cycle cursor changed")

stimulus = sqlite3.connect(Path(stimulus_path))
stimuli = stimulus.execute("SELECT COUNT(*) FROM local_goose_cycle_stimuli").fetchone()[0]
stimulus.close()
if stimuli != expected_stimuli:
    raise SystemExit("copied stimulus ledger changed")

thread = sqlite3.connect(Path(thread_path))
version = thread.execute("PRAGMA user_version").fetchone()[0]
head = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_head WHERE alias=?",
    (thread_alias,),
).fetchone()
history = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_history WHERE alias=? ORDER BY revision",
    (thread_alias,),
).fetchall()
thread.close()
if version != 2:
    raise SystemExit(f"thread sidecar schema differs: {version}")
if head != (3, root_id, feedback_id):
    raise SystemExit(f"preferred thread head differs: {head!r}")
expected_history = [
    (0, root_id, v2_head_id),
    (1, root_id, human_id),
    (2, root_id, observation_id),
    (3, root_id, feedback_id),
]
if history != expected_history:
    raise SystemExit(f"preferred thread history differs: {history!r}")

feed = sqlite3.connect(Path(feed_path))
feed_version = feed.execute("PRAGMA user_version").fetchone()[0]
events = feed.execute(
    "SELECT sequence,thread_revision,task_id,root_task_id,turn_index,request_id,"
    "speaker_kind,speaker_id,message_kind,result_sha256,response "
    "FROM local_goose_dialogue_completion_event "
    "WHERE thread_alias=? ORDER BY sequence",
    (thread_alias,),
).fetchall()
materialization = feed.execute(
    "SELECT next_revision FROM local_goose_dialogue_materialization "
    "WHERE thread_alias=?",
    (thread_alias,),
).fetchone()
consumer_count = feed.execute(
    "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor"
).fetchone()[0]
feed.close()
if feed_version != 1:
    raise SystemExit(f"completion feed schema differs: {feed_version}")
if len(events) != 4:
    raise SystemExit(f"expected four completion events, got {len(events)}")
if materialization != (4,):
    raise SystemExit(f"completion materialization cursor differs: {materialization!r}")
if consumer_count != 0:
    raise SystemExit("completion feed was implicitly consumed")

expected_events = [
    (0, v2_head_id, 1, "first-production-dialogue-v2-turn-1", "human", "legacy-v2-human", "dialogue", v2_head_sha),
    (1, human_id, 2, human_request, "human", "bertrand", "dialogue", result_hashes[human_id]),
    (2, observation_id, 3, observation_request, "system", "gaudere-runtime", "observation", result_hashes[observation_id]),
    (3, feedback_id, 4, feedback_request, "system", "sol", "feedback", result_hashes[feedback_id]),
]
for expected_sequence, (event, expected_event) in enumerate(zip(events, expected_events), start=1):
    sequence, revision, task_id, event_root, turn_index, request_id, speaker_kind, speaker_id, message_kind, result_sha, response = event
    exp_revision, exp_task, exp_turn, exp_request, exp_speaker_kind, exp_speaker_id, exp_message_kind, exp_sha = expected_event
    if sequence != expected_sequence:
        raise SystemExit(f"completion feed sequence differs: {sequence} != {expected_sequence}")
    if (
        revision != exp_revision
        or task_id != exp_task
        or event_root != root_id
        or turn_index != exp_turn
        or request_id != exp_request
        or speaker_kind != exp_speaker_kind
        or speaker_id != exp_speaker_id
        or message_kind != exp_message_kind
        or result_sha != exp_sha
        or not response
    ):
        raise SystemExit(f"completion feed event differs at revision {exp_revision}: {event!r}")

print(f"HUMAN_V3_RESPONSE={responses[human_id]}")
print(f"SYSTEM_OBSERVATION_V3_RESPONSE={responses[observation_id]}")
print(f"SYSTEM_FEEDBACK_V3_RESPONSE={responses[feedback_id]}")
print(f"HUMAN_V3_RESULT_SHA256={result_hashes[human_id]}")
print(f"SYSTEM_OBSERVATION_V3_RESULT_SHA256={result_hashes[observation_id]}")
print(f"SYSTEM_FEEDBACK_V3_RESULT_SHA256={result_hashes[feedback_id]}")
PY

[ "$("$systemctl_command" --user is-active "$service_name")" = "active" ] || fail "production service stopped during isolated proof"
production_image_after=$("$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://')
production_network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
provider_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
production_cursor_after=$(sqlite3 -readonly "$cycle_sidecar" "$cursor_query")
production_cycle_tasks_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
production_v1_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';")
production_v2_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v2';")
production_v3_after=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")
production_stimuli_after=$(sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")

[ "$production_image_after" = "$production_image_before" ] || fail "production image changed during isolated proof"
[ "$production_network_after" = "$production_network_before" ] || fail "production network changed during isolated proof"
[ "$provider_after" = "$provider_before" ] || fail "production provider total changed during isolated proof"
[ "$production_cursor_after" = "$production_cursor_before" ] || fail "production cycle cursor changed during isolated proof"
[ "$production_cycle_tasks_after" = "$production_cycle_tasks_before" ] || fail "production Local Goose cycle Task count changed"
[ "$production_v1_after" = "$production_v1_before" ] || fail "production V1 dialogue Task count changed"
[ "$production_v2_after" = "$production_v2_before" ] || fail "production V2 dialogue Task count changed"
[ "$production_v3_after" = "$production_v3_before" ] || fail "production V3 dialogue Task count changed"
[ "$production_stimuli_after" = "$production_stimuli_before" ] || fail "production stimulus ledger changed"

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$production_image_after"
printf 'PRODUCTION_NETWORK=%s\n' "$production_network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$production_cursor_after"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$production_cycle_tasks_after"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$production_v1_after"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$production_v2_after"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$production_v3_after"
printf 'STIMULI=%s\n' "$production_stimuli_after"
printf 'ISOLATED_HUMAN_V3_TASK=%s\n' "$human_task"
printf 'ISOLATED_SYSTEM_OBSERVATION_V3_TASK=%s\n' "$observation_task"
printf 'ISOLATED_SYSTEM_FEEDBACK_V3_TASK=%s\n' "$feedback_task"
printf 'FEDORA_LOCAL_GOOSE_DIALOGUE_V3_MULTI_ACTOR_ISOLATED=PASS\n'

proof_ok=1
