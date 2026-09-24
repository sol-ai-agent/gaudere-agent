#!/bin/sh
set -eu

authorization=${GAUDERE_FIRST_LOCAL_GOOSE_DIALOGUE_V2_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-95660bd47c52de5bbac298047055f6b390018c6f86bee7457e59a88d18f0e3b6}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
request_id="first-production-dialogue-v2"
message="Bonjour Gaudere. C'est Bertrand. Ceci est notre premier échange direct en dialogue V2 multi-tour en production. Peux-tu confirmer que tu me reçois et que ce tour ouvre bien un nouveau fil ?"

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"

fail()
{
    printf 'gaudere first production Local Goose dialogue v2: FAIL: %s\n' "$*" >&2
    exit 1
}

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

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

v1_dialogue_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';"
}

v2_dialogue_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v2';"
}

cycle_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';"
}

stimulus_count()
{
    sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;"
}

[ "$authorization" = "AUTHORIZED_FIRST_PRODUCTION_DIALOGUE_V2" ] || fail "explicit first-production V2 dialogue authorization token is required"

for command in "$podman_command" "$systemctl_command" sqlite3 python3 sed grep sleep; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

[ "$(service_state)" = "active" ] || fail "production service is not active"
image_before=$(running_image)
[ "$image_before" = "$expected_image" ] || fail "production image differs from the approved V2 dialogue image"
network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production network is not none"

provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total is $provider_before, expected $expected_provider_total"
cursor_before=$(cycle_cursor)
[ "$cursor_before" = "$expected_cycle_cursor" ] || fail "Local Goose autonomous cycle differs from the expected dormant cursor"
cycle_tasks_before=$(cycle_task_count)
[ "$cycle_tasks_before" = "1" ] || fail "production Local Goose cycle Task count is not exactly 1"
v1_dialogue_before=$(v1_dialogue_task_count)
[ "$v1_dialogue_before" = "1" ] || fail "production V1 dialogue Task count is not exactly 1"
stimuli_before=$(stimulus_count)
[ "$stimuli_before" = "0" ] || fail "stimulus ledger is not empty"

v2_dialogue_before=$(v2_dialogue_task_count)
case "$v2_dialogue_before" in
    0) ;;
    1)
        python3 - "$state_database" "$request_id" "$message" "$model_sha256" <<'PY'
import hashlib
import json
import sqlite3
import sys

db, request_id, message, model_sha = sys.argv[1:]
con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
rows = con.execute(
    "SELECT id,input FROM tasks WHERE kind='cognition.local-goose-dialogue.v2'"
).fetchall()
con.close()
if len(rows) != 1:
    raise SystemExit("existing V2 dialogue Task count changed during validation")
task_id, raw = rows[0]
doc = json.loads(raw)
expected = {
    "message",
    "model_sha256",
    "predecessor_result_sha256",
    "predecessor_task_id",
    "request_id",
    "root_task_id",
    "schema",
    "turn_index",
}
if set(doc) != expected:
    raise SystemExit("existing V2 root input keys differ")
if (
    doc["schema"] != "gaudere.cognition.local-goose-dialogue.v2"
    or doc["request_id"] != request_id
    or doc["message"] != message
    or doc["model_sha256"] != model_sha
    or doc["turn_index"] != 0
    or doc["root_task_id"] != ""
    or doc["predecessor_task_id"] != ""
    or doc["predecessor_result_sha256"] != ""
):
    raise SystemExit("existing V2 Task is not the canonical first-production root")
expected_id = "cognition.local-goose-dialogue.v2:" + hashlib.sha256(raw.encode("utf-8")).hexdigest()
if task_id != expected_id:
    raise SystemExit("existing V2 root Task id differs from canonical input hash")
PY
        ;;
    *) fail "production contains more than one Local Goose dialogue V2 Task before first root" ;;
esac

printf '=== FIRST PRODUCTION LOCAL DIALOGUE V2 ROOT ===\n'
printf 'REQUEST_ID=%s\n' "$request_id"
printf 'MESSAGE=%s\n' "$message"

submit_output=$("$podman_command" exec gaudere-agent     /usr/local/bin/gaudere-control     --socket /tmp/gaudere-control.sock     local-thread-start "$request_id" "$message") || fail "local-thread-start submission failed"
printf '%s\n' "$submit_output"

task_id=$(printf '%s\n' "$submit_output" | sed -n 's/^id="\(.*\)"$/\1/p' | head -n 1)
[ -n "$task_id" ] || fail "could not resolve V2 root Task id from submission"

attempt=0
status=""
task_output=""
while [ "$attempt" -lt 180 ]; do
    attempt=$((attempt + 1))
    task_output=$("$podman_command" exec gaudere-agent         /usr/local/bin/gaudere-control         --socket /tmp/gaudere-control.sock         task "$task_id") || fail "V2 root Task inspection failed"
    status=$(printf '%s\n' "$task_output" | sed -n 's/^status=//p' | head -n 1)
    case "$status" in
        succeeded|failed|cancelled|manual_review) break ;;
    esac
    sleep 1
done

[ "$status" = "succeeded" ] || {
    printf '%s\n' "$task_output" >&2
    fail "V2 root Task did not succeed; terminal/current status=$status"
}

python3 - "$state_database" "$task_id" "$request_id" "$message" "$model_sha256" <<'PY'
import hashlib
import json
import sqlite3
import sys

db, task_id, request_id, message, model_sha = sys.argv[1:]
con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
row = con.execute(
    "SELECT input,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE id=?",
    (task_id,),
).fetchone()
con.close()
if row is None:
    raise SystemExit("V2 root Task disappeared from durable state")
raw_input, status, attempts, content_type, output, failure_code, failure_message = row
if status != 3 or attempts != 1:
    raise SystemExit(f"V2 root Task did not succeed exactly once: status={status} attempts={attempts}")
if content_type != "application/vnd.gaudere.local-goose-dialogue-v2-response+json":
    raise SystemExit(f"unexpected V2 result content type: {content_type!r}")
if failure_code or failure_message:
    raise SystemExit("V2 root Task contains failure evidence")

input_doc = json.loads(raw_input)
expected_input_keys = {
    "message",
    "model_sha256",
    "predecessor_result_sha256",
    "predecessor_task_id",
    "request_id",
    "root_task_id",
    "schema",
    "turn_index",
}
if set(input_doc) != expected_input_keys:
    raise SystemExit("V2 root input keys differ")
if (
    input_doc["schema"] != "gaudere.cognition.local-goose-dialogue.v2"
    or input_doc["request_id"] != request_id
    or input_doc["message"] != message
    or input_doc["model_sha256"] != model_sha
    or input_doc["turn_index"] != 0
    or input_doc["root_task_id"] != ""
    or input_doc["predecessor_task_id"] != ""
    or input_doc["predecessor_result_sha256"] != ""
):
    raise SystemExit("V2 root input evidence differs")
expected_task_id = "cognition.local-goose-dialogue.v2:" + hashlib.sha256(raw_input.encode("utf-8")).hexdigest()
if task_id != expected_task_id:
    raise SystemExit("V2 root Task id differs from canonical input hash")

doc = json.loads(output)
expected_result_keys = {
    "model_sha256",
    "predecessor_result_sha256",
    "predecessor_task_id",
    "request_id",
    "response",
    "root_task_id",
    "schema",
    "turn_index",
}
if set(doc) != expected_result_keys:
    raise SystemExit("V2 root result keys differ")
if (
    doc["schema"] != "gaudere.cognition.local-goose-dialogue-v2-response.v1"
    or doc["request_id"] != request_id
    or doc["model_sha256"] != model_sha
    or doc["root_task_id"] != task_id
    or doc["turn_index"] != 0
    or doc["predecessor_task_id"] != ""
    or doc["predecessor_result_sha256"] != ""
):
    raise SystemExit("V2 root response lineage/identity differs")
if not isinstance(doc["response"], str) or not doc["response"].strip():
    raise SystemExit("V2 root response is empty")
if len(doc["response"].encode("utf-8")) > 16 * 1024:
    raise SystemExit("V2 root response exceeds the V2 bound")
print(f"DIALOGUE_V2_RESPONSE={doc['response']}")
print(f"DIALOGUE_V2_RESULT_SHA256={hashlib.sha256(output.encode('utf-8')).hexdigest()}")
PY

printf '=== VERIFY PRODUCTION PRESERVATION ===\n'
[ "$(service_state)" = "active" ] || fail "production service is not active after V2 root"
image_after=$(running_image)
[ "$image_after" = "$image_before" ] || fail "production image changed during V2 root"
network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "$network_before" ] || fail "production network changed during V2 root"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during local V2 dialogue"
cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ] || fail "autonomous cycle cursor changed during local V2 dialogue"
cycle_tasks_after=$(cycle_task_count)
[ "$cycle_tasks_after" = "$cycle_tasks_before" ] || fail "autonomous Local Goose Task count changed during V2 root"
stimuli_after=$(stimulus_count)
[ "$stimuli_after" = "$stimuli_before" ] || fail "stimulus ledger changed during V2 root"
v1_dialogue_after=$(v1_dialogue_task_count)
[ "$v1_dialogue_after" = "$v1_dialogue_before" ] || fail "V1 dialogue Task count changed during V2 root"
v2_dialogue_after=$(v2_dialogue_task_count)
[ "$v2_dialogue_after" = "1" ] || fail "expected exactly one durable production V2 dialogue Task"

printf 'TASK_ID=%s\n' "$task_id"
printf 'PRODUCTION_IMAGE=%s\n' "$image_after"
printf 'PRODUCTION_NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$cursor_after"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$cycle_tasks_after"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$v1_dialogue_after"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$v2_dialogue_after"
printf 'STIMULI=%s\n' "$stimuli_after"
printf 'FIRST_PRODUCTION_LOCAL_GOOSE_DIALOGUE_V2=PASS\n'
