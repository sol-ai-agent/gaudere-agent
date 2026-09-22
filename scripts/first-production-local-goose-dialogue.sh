#!/bin/sh
set -eu

authorization=${GAUDERE_FIRST_LOCAL_GOOSE_DIALOGUE_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-c83667ebd552032dcc92c62516bbfe7b22d91ac87432d63a7ffd4c0ba287a897}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
request_id="first-production-dialogue-v1"
message="Bonjour Gaudere. C'est Bertrand. Ceci est notre premier échange direct en production. Peux-tu confirmer que tu me reçois ?"

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
    printf 'gaudere first production Local Goose dialogue: FAIL: %s\n' "$*" >&2
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

dialogue_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';"
}

cycle_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';"
}

stimulus_count()
{
    sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;"
}

[ "$authorization" = "AUTHORIZED_FIRST_PRODUCTION_DIALOGUE" ] || fail "explicit first-dialogue authorization token is required"

for command in "$podman_command" "$systemctl_command" sqlite3 python3 sed grep sleep; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

[ "$(service_state)" = "active" ] || fail "production service is not active"
image_before=$(running_image)
[ "$image_before" = "$expected_image" ] || fail "production image differs from the approved dialogue image"
network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production network is not none"

provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total is $provider_before, expected $expected_provider_total"
cursor_before=$(cycle_cursor)
[ "$cursor_before" = "$expected_cycle_cursor" ] || fail "Local Goose autonomous cycle differs from the expected dormant cursor"
cycle_tasks_before=$(cycle_task_count)
stimuli_before=$(stimulus_count)
[ "$stimuli_before" = "0" ] || fail "stimulus ledger is not empty"

dialogue_before=$(dialogue_task_count)
case "$dialogue_before" in
    0) ;;
    1)
        python3 - "$state_database" "$request_id" "$message" "$model_sha256" <<'PY'
import json
import sqlite3
import sys

db, request_id, message, model_sha = sys.argv[1:]
con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
rows = con.execute(
    "SELECT input FROM tasks WHERE kind='cognition.local-goose-dialogue.v1'"
).fetchall()
con.close()
if len(rows) != 1:
    raise SystemExit("existing dialogue Task count changed during validation")
doc = json.loads(rows[0][0])
if doc.get("request_id") != request_id or doc.get("message") != message or doc.get("model_sha256") != model_sha:
    raise SystemExit("existing dialogue Task is not the canonical first-dialogue request")
PY
        ;;
    *) fail "production contains more than one Local Goose dialogue Task" ;;
esac

printf '=== FIRST PRODUCTION LOCAL DIALOGUE ===\n'
printf 'REQUEST_ID=%s\n' "$request_id"
printf 'MESSAGE=%s\n' "$message"

submit_output=$("$podman_command" exec gaudere-agent     /usr/local/bin/gaudere-control     --socket /tmp/gaudere-control.sock     local-message "$request_id" "$message") || fail "local-message submission failed"
printf '%s\n' "$submit_output"

task_id=$(printf '%s\n' "$submit_output" | sed -n 's/^id="\(.*\)"$/\1/p' | head -n 1)
[ -n "$task_id" ] || fail "could not resolve dialogue Task id from submission"

attempt=0
status=""
while [ "$attempt" -lt 180 ]; do
    attempt=$((attempt + 1))
    task_output=$("$podman_command" exec gaudere-agent         /usr/local/bin/gaudere-control         --socket /tmp/gaudere-control.sock         task "$task_id") || fail "dialogue Task inspection failed"
    status=$(printf '%s\n' "$task_output" | sed -n 's/^status=//p' | head -n 1)
    case "$status" in
        succeeded|failed|cancelled|manual_review) break ;;
    esac
    sleep 1
done

[ "$status" = "succeeded" ] || {
    printf '%s\n' "$task_output" >&2
    fail "dialogue Task did not succeed; terminal/current status=$status"
}

python3 - "$state_database" "$task_id" "$request_id" "$model_sha256" <<'PY'
import json
import sqlite3
import sys

db, task_id, request_id, model_sha = sys.argv[1:]
con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
row = con.execute(
    "SELECT status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE id=?",
    (task_id,),
).fetchone()
con.close()
if row is None:
    raise SystemExit("dialogue Task disappeared from durable state")
status, attempts, content_type, output, failure_code, failure_message = row
if status != 3 or attempts != 1:
    raise SystemExit(f"dialogue Task did not succeed exactly once: status={status} attempts={attempts}")
if content_type != "application/vnd.gaudere.local-goose-dialogue-response+json":
    raise SystemExit(f"unexpected dialogue result content type: {content_type!r}")
if failure_code or failure_message:
    raise SystemExit("dialogue Task contains failure evidence")
doc = json.loads(output)
if set(doc) != {"model_sha256", "request_id", "response", "schema"}:
    raise SystemExit("dialogue result keys differ from the canonical response")
if doc["schema"] != "gaudere.cognition.local-goose-dialogue-response.v1":
    raise SystemExit("dialogue response schema differs")
if doc["request_id"] != request_id or doc["model_sha256"] != model_sha:
    raise SystemExit("dialogue response identity differs")
if not isinstance(doc["response"], str) or not doc["response"].strip():
    raise SystemExit("dialogue response is empty")
if len(doc["response"].encode("utf-8")) > 16 * 1024:
    raise SystemExit("dialogue response exceeds the v1 bound")
print(f"DIALOGUE_RESPONSE={doc['response']}")
PY

printf '=== VERIFY PRODUCTION PRESERVATION ===\n'
[ "$(service_state)" = "active" ] || fail "production service is not active after dialogue"
image_after=$(running_image)
[ "$image_after" = "$image_before" ] || fail "production image changed during dialogue"
network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "$network_before" ] || fail "production network changed during dialogue"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during local dialogue"
cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ] || fail "autonomous cycle cursor changed during local dialogue"
cycle_tasks_after=$(cycle_task_count)
[ "$cycle_tasks_after" = "$cycle_tasks_before" ] || fail "autonomous Local Goose Task count changed during dialogue"
stimuli_after=$(stimulus_count)
[ "$stimuli_after" = "$stimuli_before" ] || fail "stimulus ledger changed during dialogue"
dialogue_after=$(dialogue_task_count)
[ "$dialogue_after" = "1" ] || fail "expected exactly one durable production dialogue Task"

printf 'TASK_ID=%s\n' "$task_id"
printf 'PRODUCTION_IMAGE=%s\n' "$image_after"
printf 'PRODUCTION_NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$cursor_after"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$cycle_tasks_after"
printf 'LOCAL_GOOSE_DIALOGUE_TASKS=%s\n' "$dialogue_after"
printf 'STIMULI=%s\n' "$stimuli_after"
printf 'FIRST_PRODUCTION_LOCAL_GOOSE_DIALOGUE=PASS\n'
