#!/bin/sh
set -eu

authorization=${GAUDERE_FIRST_LOCAL_GOOSE_DIALOGUE_V3_SYSTEM_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-58de3d5fc9fa5e20abe6a50018ba0a11804a3299f3deb6033487ed0c5c9eae9c}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
root_task_id="cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19"
predecessor_task_id="cognition.local-goose-dialogue.v2:3723223592d31a36116fd30da83a49c23519347313e1fb1d90127dbd4d0da11d"
predecessor_result_sha256="0bbd1857634f2c6502d8700c3e7819f48bce071d7c1c4267146d1308469e88a0"
thread_alias="main"
request_id="first-production-dialogue-v3-system-observation"
speaker_kind="system"
speaker_id="sol"
message_kind="observation"
message="Bonjour Gaudere. Ici Sol, composant système. Le canal de dialogue V3 multi-acteur est maintenant actif en production. Peux-tu confirmer que tu identifies ce message comme provenant du système, et non de Bertrand, tout en conservant la continuité du fil ouvert avec lui ?"

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
thread_sidecar="$state_directory/local-goose-dialogue-thread.db"
completion_sidecar="$state_directory/local-goose-dialogue-completion.db"

fail()
{
    printf 'gaudere first production Local Goose dialogue v3 system turn: FAIL: %s\n' "$*" >&2
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

task_count()
{
    kind=$1
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='$kind';"
}

stimulus_count()
{
    sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;"
}

control()
{
    "$podman_command" exec gaudere-agent         /usr/local/bin/gaudere-control         --socket /tmp/gaudere-control.sock "$@"
}

[ "$authorization" = "AUTHORIZED_FIRST_PRODUCTION_DIALOGUE_V3_SYSTEM" ] || fail "explicit first-production V3 system-dialogue authorization token is required"

for command in "$podman_command" "$systemctl_command" sqlite3 python3 sed grep sleep stat; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

[ "$(stat -c '%a' "$thread_sidecar")" = "600" ] || fail "dialogue thread sidecar mode is not 0600"
[ "$(stat -c '%a' "$completion_sidecar")" = "600" ] || fail "dialogue completion sidecar mode is not 0600"
[ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "dialogue thread sidecar schema is not 2"
[ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "dialogue completion sidecar schema is not 1"

[ "$(service_state)" = "active" ] || fail "production service is not active"
image_before=$(running_image)
[ "$image_before" = "$expected_image" ] || fail "production image differs from approved Stage 9G image"
network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production network is not none"
provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total is $provider_before, expected $expected_provider_total"
cursor_before=$(cycle_cursor)
[ "$cursor_before" = "$expected_cycle_cursor" ] || fail "Local Goose autonomous cycle differs from expected dormant cursor"
cycle_tasks_before=$(task_count cognition.local-goose-cycle.v1)
v1_before=$(task_count cognition.local-goose-dialogue.v1)
v2_before=$(task_count cognition.local-goose-dialogue.v2)
v3_before=$(task_count cognition.local-goose-dialogue.v3)
stimuli_before=$(stimulus_count)

[ "$cycle_tasks_before" = "1" ] || fail "production Local Goose cycle Task count is not exactly 1"
[ "$v1_before" = "1" ] || fail "production V1 dialogue Task count is not exactly 1"
[ "$v2_before" = "2" ] || fail "production V2 dialogue Task count is not exactly 2"
case "$v3_before" in
    0|1) ;;
    *) fail "production V3 dialogue Task count is neither 0 nor canonical-rerun candidate 1" ;;
esac
[ "$stimuli_before" = "0" ] || fail "production stimulus ledger is not empty"

python3 -     "$state_database"     "$thread_sidecar"     "$completion_sidecar"     "$root_task_id"     "$predecessor_task_id"     "$predecessor_result_sha256"     "$request_id"     "$speaker_kind"     "$speaker_id"     "$message_kind"     "$message"     "$model_sha256"     "$v3_before" <<'PY'
import hashlib
import json
import sqlite3
import sys

(
    state_path,
    thread_path,
    completion_path,
    root_id,
    predecessor_id,
    predecessor_result_sha,
    request_id,
    speaker_kind,
    speaker_id,
    message_kind,
    message,
    model_sha,
    v3_before,
) = sys.argv[1:]
v3_before = int(v3_before)

state = sqlite3.connect(f"file:{state_path}?mode=ro", uri=True)
row = state.execute(
    "SELECT input,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE id=? AND kind='cognition.local-goose-dialogue.v2'",
    (predecessor_id,),
).fetchone()
if row is None:
    raise SystemExit("canonical Stage 8 predecessor is missing")
raw_input, status, attempts, content_type, raw_result, failure_code, failure_message = row
if status != 3 or attempts != 1:
    raise SystemExit("Stage 8 predecessor is not a single canonical success")
if content_type != "application/vnd.gaudere.local-goose-dialogue-v2-response+json":
    raise SystemExit("Stage 8 predecessor result content type differs")
if failure_code or failure_message:
    raise SystemExit("Stage 8 predecessor contains failure evidence")
inp = json.loads(raw_input)
out = json.loads(raw_result)
if (
    inp.get("schema") != "gaudere.cognition.local-goose-dialogue.v2"
    or inp.get("turn_index") != 1
    or inp.get("root_task_id") != root_id
    or inp.get("predecessor_task_id") != root_id
    or inp.get("model_sha256") != model_sha
    or out.get("schema") != "gaudere.cognition.local-goose-dialogue-v2-response.v1"
    or out.get("turn_index") != 1
    or out.get("root_task_id") != root_id
    or out.get("predecessor_task_id") != root_id
    or out.get("model_sha256") != model_sha
):
    raise SystemExit("Stage 8 predecessor lineage/model differs")
actual = hashlib.sha256(raw_result.encode("utf-8")).hexdigest()
if actual != predecessor_result_sha:
    raise SystemExit(f"Stage 8 predecessor result SHA differs: {actual}")

v3_rows = state.execute(
    "SELECT id,input,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE kind='cognition.local-goose-dialogue.v3'"
).fetchall()
state.close()
if len(v3_rows) != v3_before:
    raise SystemExit("V3 Task count changed during preflight")

thread = sqlite3.connect(f"file:{thread_path}?mode=ro", uri=True)
heads = thread.execute(
    "SELECT alias,revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_head"
).fetchall()
history = thread.execute(
    "SELECT alias,revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_history ORDER BY alias,revision"
).fetchall()
thread.close()

completion = sqlite3.connect(f"file:{completion_path}?mode=ro", uri=True)
events = completion.execute(
    "SELECT thread_alias,thread_revision,task_id FROM local_goose_dialogue_completion_event ORDER BY sequence"
).fetchall()
materialization = completion.execute(
    "SELECT thread_alias,next_revision FROM local_goose_dialogue_materialization"
).fetchall()
consumers = completion.execute(
    "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor"
).fetchone()[0]
completion.close()
if consumers != 0:
    raise SystemExit("completion feed already has a consumer cursor before first V3 system turn")

if v3_before == 0:
    allowed_empty = heads == [] and history == [] and events == [] and materialization == []
    allowed_bound = (
        heads == [("main", 0, root_id, predecessor_id)]
        and history == [("main", 0, root_id, predecessor_id)]
        and (
            (events == [] and materialization == [])
            or (
                events == [("main", 0, predecessor_id)]
                and materialization == [("main", 1)]
            )
        )
    )
    if not (allowed_empty or allowed_bound):
        raise SystemExit(
            f"preflight sidecars differ for first V3 system turn: "
            f"heads={heads!r} history={history!r} events={events!r} "
            f"materialization={materialization!r}"
        )
else:
    if len(v3_rows) != 1:
        raise SystemExit("rerun candidate V3 Task count differs")
    task_id, raw_v3_input, v3_status, v3_attempts, v3_content_type, v3_result, v3_failure_code, v3_failure_message = v3_rows[0]
    doc = json.loads(raw_v3_input)
    expected_keys = {
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
    if set(doc) != expected_keys:
        raise SystemExit("existing V3 rerun candidate input keys differ")
    if (
        doc.get("schema") != "gaudere.cognition.local-goose-dialogue.v3"
        or doc.get("request_id") != request_id
        or doc.get("speaker_kind") != speaker_kind
        or doc.get("speaker_id") != speaker_id
        or doc.get("message_kind") != message_kind
        or doc.get("message") != message
        or doc.get("model_sha256") != model_sha
        or doc.get("turn_index") != 2
        or doc.get("root_task_id") != root_id
        or doc.get("predecessor_task_id") != predecessor_id
        or doc.get("predecessor_result_sha256") != predecessor_result_sha
    ):
        raise SystemExit("existing V3 rerun candidate does not match canonical Stage 9H request")
    expected_task_id = "cognition.local-goose-dialogue.v3:" + hashlib.sha256(
        raw_v3_input.encode("utf-8")
    ).hexdigest()
    if task_id != expected_task_id:
        raise SystemExit("existing V3 rerun candidate Task identity differs")
    if heads != [("main", 1, root_id, task_id)]:
        raise SystemExit("existing V3 rerun candidate preferred head differs")
    if history != [
        ("main", 0, root_id, predecessor_id),
        ("main", 1, root_id, task_id),
    ]:
        raise SystemExit("existing V3 rerun candidate thread history differs")
PY

if [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "0" ]; then
    printf '=== BIND PRODUCTION V3 PREFERRED THREAD ===\n'
    bind_output=$(control local-thread-v3-bind "$thread_alias" "$predecessor_task_id") || fail "preferred thread bind failed"
    printf '%s\n' "$bind_output"
    printf '%s\n' "$bind_output" | grep -q "^alias=\"$thread_alias\"$" || fail "bound alias differs"
    printf '%s\n' "$bind_output" | grep -q '^revision=0$' || fail "bound preferred revision is not zero"
    printf '%s\n' "$bind_output" | grep -q "^root_task_id=\"$root_task_id\"$" || fail "bound preferred root differs"
    printf '%s\n' "$bind_output" | grep -q "^head_task_id=\"$predecessor_task_id\"$" || fail "bound preferred head differs"
fi

head_output=$(control local-thread-v3-head "$thread_alias") || fail "preferred thread head inspection failed"
printf '%s\n' "$head_output"
head_revision=$(printf '%s\n' "$head_output" | sed -n 's/^revision=//p' | head -n 1)
case "$head_revision" in
    0|1) ;;
    *) fail "preferred head revision is neither pre-effect 0 nor canonical rerun 1" ;;
esac

printf '=== FIRST PRODUCTION LOCAL DIALOGUE V3 SYSTEM TURN ===\n'
printf 'REQUEST_ID=%s\n' "$request_id"
printf 'THREAD_ALIAS=%s\n' "$thread_alias"
printf 'EXPECTED_REVISION=0\n'
printf 'SPEAKER_KIND=%s\n' "$speaker_kind"
printf 'SPEAKER_ID=%s\n' "$speaker_id"
printf 'MESSAGE_KIND=%s\n' "$message_kind"
printf 'MESSAGE=%s\n' "$message"

submit_output=$(control local-thread-v3-send     "$request_id" "$thread_alias" 0     "$speaker_kind" "$speaker_id" "$message_kind" "$message") || fail "first production V3 system submission failed"
printf '%s\n' "$submit_output"

task_id=$(printf '%s\n' "$submit_output" | sed -n 's/^id="\(.*\)"$/\1/p' | head -n 1)
[ -n "$task_id" ] || fail "could not resolve first production V3 system Task id"

attempt=0
status=""
task_output=""
while [ "$attempt" -lt 180 ]; do
    attempt=$((attempt + 1))
    task_output=$(control task "$task_id") || fail "V3 system Task inspection failed"
    status=$(printf '%s\n' "$task_output" | sed -n 's/^status=//p' | head -n 1)
    case "$status" in
        succeeded|failed|cancelled|manual_review) break ;;
    esac
    sleep 1
done
[ "$status" = "succeeded" ] || {
    printf '%s\n' "$task_output" >&2
    fail "V3 system Task did not succeed; terminal/current status=$status"
}

feed_attempt=0
while [ "$feed_attempt" -lt 30 ]; do
    feed_attempt=$((feed_attempt + 1))
    completion_events=$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE thread_alias='$thread_alias';")
    materialization_next=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")
    if [ "$completion_events" = "2" ] && [ "$materialization_next" = "2" ]; then
        break
    fi
    sleep 1
done
[ "$completion_events" = "2" ] || fail "completion feed did not materialize two canonical revisions"
[ "$materialization_next" = "2" ] || fail "completion materialization cursor did not advance to 2"

python3 -     "$state_database"     "$thread_sidecar"     "$completion_sidecar"     "$task_id"     "$root_task_id"     "$predecessor_task_id"     "$predecessor_result_sha256"     "$request_id"     "$speaker_kind"     "$speaker_id"     "$message_kind"     "$message"     "$model_sha256" <<'PY'
import hashlib
import json
import sqlite3
import sys

(
    state_path,
    thread_path,
    completion_path,
    task_id,
    root_id,
    predecessor_id,
    predecessor_result_sha,
    request_id,
    speaker_kind,
    speaker_id,
    message_kind,
    message,
    model_sha,
) = sys.argv[1:]

state = sqlite3.connect(f"file:{state_path}?mode=ro", uri=True)
row = state.execute(
    "SELECT input,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE id=? AND kind='cognition.local-goose-dialogue.v3'",
    (task_id,),
).fetchone()
v3_count = state.execute(
    "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3'"
).fetchone()[0]
state.close()
if row is None:
    raise SystemExit("first production V3 system Task disappeared")
if v3_count != 1:
    raise SystemExit(f"expected exactly one production V3 Task, got {v3_count}")
raw_input, status, attempts, content_type, raw_result, failure_code, failure_message = row
if status != 3 or attempts != 1:
    raise SystemExit(f"V3 system Task did not succeed exactly once: status={status} attempts={attempts}")
if content_type != "application/vnd.gaudere.local-goose-dialogue-v3-response+json":
    raise SystemExit(f"unexpected V3 result content type: {content_type!r}")
if failure_code or failure_message:
    raise SystemExit("V3 system Task contains failure evidence")

inp = json.loads(raw_input)
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
if set(inp) != expected_input_keys:
    raise SystemExit("V3 system input keys differ")
if (
    inp.get("schema") != "gaudere.cognition.local-goose-dialogue.v3"
    or inp.get("request_id") != request_id
    or inp.get("speaker_kind") != speaker_kind
    or inp.get("speaker_id") != speaker_id
    or inp.get("message_kind") != message_kind
    or inp.get("message") != message
    or inp.get("model_sha256") != model_sha
    or inp.get("turn_index") != 2
    or inp.get("root_task_id") != root_id
    or inp.get("predecessor_task_id") != predecessor_id
    or inp.get("predecessor_result_sha256") != predecessor_result_sha
):
    raise SystemExit("V3 system input evidence differs")
expected_task_id = "cognition.local-goose-dialogue.v3:" + hashlib.sha256(
    raw_input.encode("utf-8")
).hexdigest()
if task_id != expected_task_id:
    raise SystemExit("V3 system Task id differs from canonical input hash")

out = json.loads(raw_result)
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
if set(out) != expected_result_keys:
    raise SystemExit("V3 system result keys differ")
if (
    out.get("schema") != "gaudere.cognition.local-goose-dialogue-v3-response.v1"
    or out.get("request_id") != request_id
    or out.get("speaker_kind") != speaker_kind
    or out.get("speaker_id") != speaker_id
    or out.get("message_kind") != message_kind
    or out.get("model_sha256") != model_sha
    or out.get("turn_index") != 2
    or out.get("root_task_id") != root_id
    or out.get("predecessor_task_id") != predecessor_id
    or out.get("predecessor_result_sha256") != predecessor_result_sha
):
    raise SystemExit("V3 system result lineage/provenance differs")
response = out.get("response")
if not isinstance(response, str) or not response.strip():
    raise SystemExit("V3 system response is empty")
if len(response.encode("utf-8")) > 16 * 1024:
    raise SystemExit("V3 system response exceeds bound")
result_sha = hashlib.sha256(raw_result.encode("utf-8")).hexdigest()

thread = sqlite3.connect(f"file:{thread_path}?mode=ro", uri=True)
head = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_head WHERE alias='main'"
).fetchone()
history = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_history WHERE alias='main' ORDER BY revision"
).fetchall()
thread.close()
if head != (1, root_id, task_id):
    raise SystemExit(f"preferred V3 thread head differs: {head!r}")
if history != [
    (0, root_id, predecessor_id),
    (1, root_id, task_id),
]:
    raise SystemExit(f"preferred V3 thread history differs: {history!r}")

completion = sqlite3.connect(f"file:{completion_path}?mode=ro", uri=True)
events = completion.execute(
    "SELECT sequence,thread_revision,task_id,root_task_id,turn_index,request_id,"
    "speaker_kind,speaker_id,message_kind,result_sha256,response "
    "FROM local_goose_dialogue_completion_event WHERE thread_alias='main' ORDER BY sequence"
).fetchall()
materialization = completion.execute(
    "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='main'"
).fetchone()
consumers = completion.execute(
    "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor"
).fetchone()[0]
completion.close()
if len(events) != 2:
    raise SystemExit(f"expected two completion events, got {len(events)}")
if materialization != (2,):
    raise SystemExit(f"completion materialization differs: {materialization!r}")
if consumers != 0:
    raise SystemExit("first production V3 system turn created a consumer cursor")

baseline, system_event = events
if not (
    baseline[0] == 1
    and baseline[1] == 0
    and baseline[2] == predecessor_id
    and baseline[3] == root_id
    and baseline[4] == 1
    and baseline[5] == "first-production-dialogue-v2-turn-1"
    and baseline[6] == "human"
    and baseline[7] == "legacy-v2-human"
    and baseline[8] == "dialogue"
    and baseline[9] == predecessor_result_sha
    and isinstance(baseline[10], str)
    and baseline[10]
):
    raise SystemExit(f"baseline V2 completion event differs: {baseline!r}")

if not (
    system_event[0] == 2
    and system_event[1] == 1
    and system_event[2] == task_id
    and system_event[3] == root_id
    and system_event[4] == 2
    and system_event[5] == request_id
    and system_event[6] == speaker_kind
    and system_event[7] == speaker_id
    and system_event[8] == message_kind
    and system_event[9] == result_sha
    and system_event[10] == response
):
    raise SystemExit(f"V3 system completion event differs: {system_event!r}")

print(f"DIALOGUE_V3_SYSTEM_RESPONSE={response}")
print(f"DIALOGUE_V3_SYSTEM_RESULT_SHA256={result_sha}")
PY

printf '=== VERIFY PRODUCTION PRESERVATION ===\n'
[ "$(service_state)" = "active" ] || fail "production service is not active after first V3 system turn"
image_after=$(running_image)
[ "$image_after" = "$image_before" ] || fail "production image changed during first V3 system turn"
network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "$network_before" ] || fail "production network changed during first V3 system turn"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during first V3 system turn"
cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ] || fail "autonomous cycle cursor changed during first V3 system turn"
cycle_tasks_after=$(task_count cognition.local-goose-cycle.v1)
v1_after=$(task_count cognition.local-goose-dialogue.v1)
v2_after=$(task_count cognition.local-goose-dialogue.v2)
v3_after=$(task_count cognition.local-goose-dialogue.v3)
stimuli_after=$(stimulus_count)

[ "$cycle_tasks_after" = "$cycle_tasks_before" ] || fail "autonomous cycle Task count changed"
[ "$v1_after" = "$v1_before" ] || fail "V1 dialogue Task count changed"
[ "$v2_after" = "$v2_before" ] || fail "V2 dialogue Task count changed"
[ "$v3_after" = "1" ] || fail "expected exactly one durable production V3 Task"
[ "$stimuli_after" = "$stimuli_before" ] || fail "stimulus ledger changed"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;")" = "0" ] || fail "a completion consumer cursor appeared unexpectedly"

printf 'TASK_ID=%s\n' "$task_id"
printf 'ROOT_TASK_ID=%s\n' "$root_task_id"
printf 'PREDECESSOR_TASK_ID=%s\n' "$predecessor_task_id"
printf 'PREDECESSOR_RESULT_SHA256=%s\n' "$predecessor_result_sha256"
printf 'THREAD_ALIAS=%s\n' "$thread_alias"
printf 'THREAD_REVISION=1\n'
printf 'PRODUCTION_IMAGE=%s\n' "$image_after"
printf 'PRODUCTION_NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$cursor_after"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$cycle_tasks_after"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$v1_after"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$v2_after"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$v3_after"
printf 'STIMULI=%s\n' "$stimuli_after"
printf 'DIALOGUE_COMPLETION_EVENTS=2\n'
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=2\n'
printf 'DIALOGUE_COMPLETION_CONSUMERS=0\n'
printf 'FIRST_PRODUCTION_LOCAL_GOOSE_DIALOGUE_V3_SYSTEM=PASS\n'
