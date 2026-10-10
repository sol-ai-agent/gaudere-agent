#!/bin/sh
set -eu

authorization=${GAUDERE_CHAT_FIRST_PRODUCTION_TURN_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-7335c2a921a46de854748b9b613f5aea74f16cbb73193643d08bf80b5c59ccd7}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
expected_responder_state_sha=${GAUDERE_EXPECTED_RESPONDER_STATE_SHA256:-77ed92357b6c7535cacf19af134bbba083e962bbd6324fadc6a5e77bf4e8d943}
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}

root_task_id=cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19
v2_head_task_id=cognition.local-goose-dialogue.v2:3723223592d31a36116fd30da83a49c23519347313e1fb1d90127dbd4d0da11d
v3_human_head_task_id=cognition.local-goose-dialogue.v3:3a28c66f9d089bccedbac2e51cd62e949905c6a7ec109423e3e9adefef52b65f
v3_system_head_task_id=cognition.local-goose-dialogue.v3:b5ff22b1696e7fd7a84005fd292a3b69d4d5ffd936cdb6507af1cb220e201813
thread_alias=main
manual_consumer_id=sol
responder_consumer_id=system-responder-v1

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
thread_sidecar="$state_directory/local-goose-dialogue-thread.db"
completion_sidecar="$state_directory/local-goose-dialogue-completion.db"
responder_sidecar="$state_directory/local-goose-dialogue-responder.db"

fail()
{
    printf 'gaudere-chat first production human turn: FAIL: %s\n' "$*" >&2
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
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='$1';"
}

stimulus_count()
{
    sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;'
}

responder_dump_sha()
{
    sqlite3 -readonly "$responder_sidecar" '.dump' | sha256sum | awk '{print $1}'
}

verify_pre_dialogue_state()
{
    for sidecar in "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
        [ -f "$sidecar" ] && [ ! -L "$sidecar" ] || fail "dialogue sidecar is missing or unsafe: $sidecar"
        [ "$(stat -c '%a' "$sidecar")" = "600" ] || fail "dialogue sidecar mode is not 0600: $sidecar"
    done

    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "dialogue thread sidecar schema is not 2"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "dialogue completion sidecar schema is not 1"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "dialogue responder sidecar schema is not 1"

    thread_heads=$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_head;')
    thread_history=$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_history;')
    thread_revision=$(sqlite3 -readonly "$thread_sidecar" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND root_task_id='$root_task_id' AND head_task_id='$v3_system_head_task_id';")
    history_zero=$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_history WHERE alias='$thread_alias' AND revision=0 AND root_task_id='$root_task_id' AND head_task_id='$v2_head_task_id';")
    history_one=$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_history WHERE alias='$thread_alias' AND revision=1 AND root_task_id='$root_task_id' AND head_task_id='$v3_human_head_task_id';")
    history_two=$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_history WHERE alias='$thread_alias' AND revision=2 AND root_task_id='$root_task_id' AND head_task_id='$v3_system_head_task_id';")
    [ "$thread_heads" = "1" ] || fail "dialogue preferred head count is not exactly 1"
    [ "$thread_history" = "3" ] || fail "dialogue thread history count is not exactly 3"
    [ "$thread_revision" = "2" ] || fail "dialogue preferred head is not canonical revision 2"
    [ "$history_zero" = "1" ] || fail "dialogue history revision 0 differs"
    [ "$history_one" = "1" ] || fail "dialogue history revision 1 differs"
    [ "$history_two" = "1" ] || fail "dialogue history revision 2 differs"

    completion_events=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')
    completion_materialization=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")
    completion_consumers=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')
    sol_sequence=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")
    responder_sequence=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")
    event_one=$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=1 AND thread_alias='$thread_alias' AND thread_revision=0 AND task_id='$v2_head_task_id';")
    event_two=$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=2 AND thread_alias='$thread_alias' AND thread_revision=1 AND task_id='$v3_human_head_task_id';")
    event_three=$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=3 AND thread_alias='$thread_alias' AND thread_revision=2 AND task_id='$v3_system_head_task_id';")
    [ "$completion_events" = "3" ] || fail "dialogue completion event count is not exactly 3"
    [ "$completion_materialization" = "3" ] || fail "dialogue completion materialization cursor is not 3"
    [ "$completion_consumers" = "2" ] || fail "dialogue completion consumer count is not exactly 2"
    [ "$sol_sequence" = "1" ] || fail "sol consumer cursor is not exactly 1"
    [ "$responder_sequence" = "2" ] || fail "responder consumer cursor is not exactly 2"
    [ "$event_one" = "1" ] || fail "completion event sequence 1 differs"
    [ "$event_two" = "1" ] || fail "completion event sequence 2 differs"
    [ "$event_three" = "1" ] || fail "completion event sequence 3 differs"

    canonical_v3=$(sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3' AND id IN ('$v3_human_head_task_id','$v3_system_head_task_id');")
    [ "$canonical_v3" = "2" ] || fail "canonical production V3 Tasks differ"

    responder_leases=$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;')
    responder_exhausted=$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE state=1 AND turns_committed=1;')
    responder_intents=$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent;')
    responder_completed=$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE state=2;')
    [ "$responder_leases" = "1" ] || fail "responder lease count is not exactly 1"
    [ "$responder_exhausted" = "1" ] || fail "responder exhausted lease state differs"
    [ "$responder_intents" = "1" ] || fail "responder intent count is not exactly 1"
    [ "$responder_completed" = "1" ] || fail "responder completed intent state differs"
}

[ "$authorization" = "AUTHORIZED_GAUDERE_CHAT_FIRST_PRODUCTION_TURN" ] || fail "explicit Stage 9K5 first-human-turn authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sed grep stat tr sha256sum awk wc sleep; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"
[ -t 0 ] || fail "Stage 9K5 requires an interactive terminal for the human message"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")

printf '=== VERIFY STAGE 9K5 PRODUCTION PRESTATE ===\n'
[ "$(service_state)" = "active" ] || fail "production service is not active"
image_before=$(running_image)
[ "$image_before" = "$expected_image" ] || fail "production image differs from approved Stage 9K4 image"
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
[ "$v3_before" = "2" ] || fail "production V3 dialogue Task count is not exactly 2"
[ "$stimuli_before" = "0" ] || fail "production stimulus ledger is not empty"
verify_pre_dialogue_state
responder_dump_before=$(responder_dump_sha)
[ "$responder_dump_before" = "$expected_responder_state_sha" ] || fail "responder state SHA differs from Stage 9K4 proof"

printf '=== VERIFY PRODUCTION GAUDERE-CHAT BEFORE HUMAN TURN ===\n'
chat_help=$("$podman_command" exec gaudere-agent /usr/local/bin/gaudere-chat --help 2>&1) || fail "production gaudere-chat --help probe failed"
printf '%s\n' "$chat_help"
printf '%s\n' "$chat_help" | grep -q '^Usage: ' || fail "production gaudere-chat usage missing"
printf '%s\n' "$chat_help" | grep -q '^/retry ' || fail "production gaudere-chat explicit retry command missing"

printf '=== STAGE 9K5: ONE REAL HUMAN PRODUCTION TURN ===\n'
printf 'Enter exactly one message for Gaudere. Local commands beginning with "/" are rejected by this gate.\n'
printf 'Bertrand> '
IFS= read -r human_message || fail "could not read the human message"
[ -n "$human_message" ] || fail "human message must not be empty"
case "$human_message" in
    /*) fail "local gaudere-chat commands are not accepted by the one-turn production gate" ;;
esac
message_bytes=$(LC_ALL=C printf '%s' "$human_message" | wc -c | tr -d ' ')
case "$message_bytes" in
    ''|*[!0-9]*) fail "could not measure human message length" ;;
esac
[ "$message_bytes" -le 4096 ] || fail "human message exceeds gaudere-chat 4096-byte bound"
message_sha256=$(printf '%s' "$human_message" | sha256sum | awk '{print $1}')
printf 'HUMAN_MESSAGE_BYTES=%s\n' "$message_bytes"
printf 'HUMAN_MESSAGE_SHA256=%s\n' "$message_sha256"

chat_output=$(
    {
        printf '%s\n' "$human_message"
        printf '/quit\n'
    } | "$podman_command" exec -i gaudere-agent /usr/local/bin/gaudere-chat --thread "$thread_alias" --verbose 2>&1
) || fail "gaudere-chat process failed"
printf '%s\n' "$chat_output"

printf '%s\n' "$chat_output" | grep -q 'Gaudere> ' || fail "gaudere-chat did not present a canonical response; do not rerun before inspection"
! printf '%s\n' "$chat_output" | grep -q 'preferred thread advanced concurrently' || fail "gaudere-chat reported a preferred-thread conflict; do not rerun before inspection"
! printf '%s\n' "$chat_output" | grep -q 'transport state is ambiguous' || fail "gaudere-chat reported ambiguous transport state; do not rerun before inspection"
! printf '%s\n' "$chat_output" | grep -q 'ended without a canonical success' || fail "gaudere-chat reported terminal failure; do not rerun before inspection"
! printf '%s\n' "$chat_output" | grep -q 'durably submitted but could not be observed as terminal' || fail "gaudere-chat timed out after durable submission; do not rerun before inspection"

printf '=== VERIFY EXACTLY ONE CANONICAL HUMAN TURN WAS ADDED ===\n'
feed_attempt=0
while [ "$feed_attempt" -lt 30 ]; do
    feed_attempt=$((feed_attempt + 1))
    completion_events_now=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')
    completion_materialization_now=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")
    if [ "$completion_events_now" = "4" ] && [ "$completion_materialization_now" = "4" ]; then
        break
    fi
    sleep 1
done
[ "$completion_events_now" = "4" ] || fail "completion feed did not reach exactly four events; do not rerun before inspection"
[ "$completion_materialization_now" = "4" ] || fail "completion materialization did not reach revision 4; do not rerun before inspection"

[ "$(service_state)" = "active" ] || fail "production service is not active after the human turn"
image_after=$(running_image)
[ "$image_after" = "$image_before" ] || fail "production image changed during the human turn"
network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "$network_before" ] || fail "production network changed during the human turn"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during the human turn"
cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ] || fail "autonomous cycle changed during the human turn"
cycle_tasks_after=$(task_count cognition.local-goose-cycle.v1)
v1_after=$(task_count cognition.local-goose-dialogue.v1)
v2_after=$(task_count cognition.local-goose-dialogue.v2)
v3_after=$(task_count cognition.local-goose-dialogue.v3)
stimuli_after=$(stimulus_count)
[ "$cycle_tasks_after" = "$cycle_tasks_before" ] || fail "cycle Task count changed"
[ "$v1_after" = "$v1_before" ] || fail "V1 dialogue Task count changed"
[ "$v2_after" = "$v2_before" ] || fail "V2 dialogue Task count changed"
[ "$v3_after" = "3" ] || fail "expected exactly one new V3 dialogue Task; do not rerun before inspection"
[ "$stimuli_after" = "$stimuli_before" ] || fail "stimulus ledger changed"

new_task_id=$(sqlite3 -readonly "$state_database" "SELECT id FROM tasks WHERE kind='cognition.local-goose-dialogue.v3' AND id NOT IN ('$v3_human_head_task_id','$v3_system_head_task_id') ORDER BY id;")
[ "$(printf '%s\n' "$new_task_id" | wc -l | tr -d ' ')" = "1" ] || fail "could not resolve exactly one new V3 human Task"
printf '%s\n' "$new_task_id" | grep -Eq '^cognition\.local-goose-dialogue\.v3:[0-9a-f]{64}$' || fail "new V3 Task id is malformed"

post_evidence=$(python3 - \
    "$state_database" \
    "$thread_sidecar" \
    "$completion_sidecar" \
    "$new_task_id" \
    "$root_task_id" \
    "$v2_head_task_id" \
    "$v3_human_head_task_id" \
    "$v3_system_head_task_id" \
    "$human_message" \
    "$model_sha256" \
    "$manual_consumer_id" \
    "$responder_consumer_id" <<'PY'
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
    v2_head_id,
    v3_human_id,
    predecessor_id,
    human_message,
    model_sha,
    manual_consumer_id,
    responder_consumer_id,
) = sys.argv[1:]

state = sqlite3.connect(f"file:{state_path}?mode=ro", uri=True)
row = state.execute(
    "SELECT input,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE id=? AND kind='cognition.local-goose-dialogue.v3'",
    (task_id,),
).fetchone()
predecessor = state.execute(
    "SELECT result_output,status,attempts_started FROM tasks "
    "WHERE id=? AND kind='cognition.local-goose-dialogue.v3'",
    (predecessor_id,),
).fetchone()
v3_count = state.execute(
    "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3'"
).fetchone()[0]
state.close()

if row is None or predecessor is None:
    raise SystemExit("new Task or canonical predecessor is missing")
if v3_count != 3:
    raise SystemExit(f"expected exactly three V3 Tasks, got {v3_count}")
raw_input, status, attempts, content_type, raw_result, failure_code, failure_message = row
predecessor_result, predecessor_status, predecessor_attempts = predecessor
if status != 3 or attempts != 1:
    raise SystemExit(f"human Task is not a single canonical success: status={status} attempts={attempts}")
if predecessor_status != 3 or predecessor_attempts != 1:
    raise SystemExit("canonical predecessor is not a single success")
if content_type != "application/vnd.gaudere.local-goose-dialogue-v3-response+json":
    raise SystemExit(f"unexpected V3 result content type: {content_type!r}")
if failure_code or failure_message:
    raise SystemExit("human Task contains failure evidence")

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
    raise SystemExit("human Task input keys differ")
predecessor_sha = hashlib.sha256(predecessor_result.encode("utf-8")).hexdigest()
request_id = inp.get("request_id")
if (
    inp.get("schema") != "gaudere.cognition.local-goose-dialogue.v3"
    or not isinstance(request_id, str)
    or not request_id.startswith("chat-v1-")
    or inp.get("speaker_kind") != "human"
    or inp.get("speaker_id") != "bertrand"
    or inp.get("message_kind") != "dialogue"
    or inp.get("message") != human_message
    or inp.get("model_sha256") != model_sha
    or inp.get("turn_index") != 4
    or inp.get("root_task_id") != root_id
    or inp.get("predecessor_task_id") != predecessor_id
    or inp.get("predecessor_result_sha256") != predecessor_sha
):
    raise SystemExit("human Task input evidence differs")
expected_task_id = "cognition.local-goose-dialogue.v3:" + hashlib.sha256(
    raw_input.encode("utf-8")
).hexdigest()
if task_id != expected_task_id:
    raise SystemExit("human Task id differs from canonical input hash")

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
    raise SystemExit("human Task result keys differ")
response = out.get("response")
if (
    out.get("schema") != "gaudere.cognition.local-goose-dialogue-v3-response.v1"
    or out.get("request_id") != request_id
    or out.get("speaker_kind") != "human"
    or out.get("speaker_id") != "bertrand"
    or out.get("message_kind") != "dialogue"
    or out.get("model_sha256") != model_sha
    or out.get("turn_index") != 4
    or out.get("root_task_id") != root_id
    or out.get("predecessor_task_id") != predecessor_id
    or out.get("predecessor_result_sha256") != predecessor_sha
    or not isinstance(response, str)
    or not response.strip()
    or len(response.encode("utf-8")) > 16 * 1024
):
    raise SystemExit("human Task result evidence differs")
result_sha = hashlib.sha256(raw_result.encode("utf-8")).hexdigest()

thread = sqlite3.connect(f"file:{thread_path}?mode=ro", uri=True)
heads = thread.execute(
    "SELECT alias,revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_head"
).fetchall()
history = thread.execute(
    "SELECT revision,root_task_id,head_task_id "
    "FROM local_goose_dialogue_thread_history WHERE alias='main' ORDER BY revision"
).fetchall()
thread.close()
if heads != [("main", 3, root_id, task_id)]:
    raise SystemExit(f"preferred head differs after human turn: {heads!r}")
if history != [
    (0, root_id, v2_head_id),
    (1, root_id, v3_human_id),
    (2, root_id, predecessor_id),
    (3, root_id, task_id),
]:
    raise SystemExit(f"thread history differs after human turn: {history!r}")

completion = sqlite3.connect(f"file:{completion_path}?mode=ro", uri=True)
events = completion.execute(
    "SELECT sequence,thread_revision,task_id,root_task_id,turn_index,request_id,"
    "speaker_kind,speaker_id,message_kind,result_sha256,response "
    "FROM local_goose_dialogue_completion_event ORDER BY sequence"
).fetchall()
materialization = completion.execute(
    "SELECT thread_alias,next_revision FROM local_goose_dialogue_materialization"
).fetchall()
consumers = completion.execute(
    "SELECT consumer_id,last_sequence FROM local_goose_dialogue_consumer_cursor "
    "ORDER BY consumer_id"
).fetchall()
completion.close()
if len(events) != 4:
    raise SystemExit(f"expected four completion events, got {len(events)}")
if materialization != [("main", 4)]:
    raise SystemExit(f"completion materialization differs: {materialization!r}")
if consumers != [(manual_consumer_id, 1), (responder_consumer_id, 2)]:
    raise SystemExit(f"completion consumers changed: {consumers!r}")
event = events[3]
if not (
    event[0] == 4
    and event[1] == 3
    and event[2] == task_id
    and event[3] == root_id
    and event[4] == 4
    and event[5] == request_id
    and event[6] == "human"
    and event[7] == "bertrand"
    and event[8] == "dialogue"
    and event[9] == result_sha
    and event[10] == response
):
    raise SystemExit(f"completion event 4 differs: {event!r}")

print(f"CHAT_TASK_ID={task_id}")
print(f"CHAT_REQUEST_ID={request_id}")
print("CHAT_MESSAGE_JSON=" + json.dumps(human_message, ensure_ascii=False))
print("CHAT_RESPONSE_JSON=" + json.dumps(response, ensure_ascii=False))
PY
) || fail "canonical post-turn evidence verification failed; do not rerun before inspection"
printf '%s\n' "$post_evidence"

responder_dump_after=$(responder_dump_sha)
[ "$responder_dump_after" = "$responder_dump_before" ] || fail "responder state changed during the human turn"

thread_revision_after=$(sqlite3 -readonly "$thread_sidecar" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")
completion_consumers_after=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')
sol_sequence_after=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")
responder_sequence_after=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$image_after"
printf 'NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$cursor_after"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$cycle_tasks_after"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$v1_after"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$v2_after"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$v3_after"
printf 'STIMULI=%s\n' "$stimuli_after"
printf 'DIALOGUE_THREAD_REVISION=%s\n' "$thread_revision_after"
printf 'DIALOGUE_COMPLETION_EVENTS=%s\n' "$completion_events_now"
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=%s\n' "$completion_materialization_now"
printf 'DIALOGUE_COMPLETION_CONSUMERS=%s\n' "$completion_consumers_after"
printf 'DIALOGUE_CONSUMER_SOL=%s:%s\n' "$manual_consumer_id" "$sol_sequence_after"
printf 'DIALOGUE_CONSUMER_RESPONDER=%s:%s\n' "$responder_consumer_id" "$responder_sequence_after"
printf 'DIALOGUE_RESPONDER_NEXT_PENDING_SEQUENCE=3\n'
printf 'RESPONDER_STATE_SHA256=%s\n' "$responder_dump_after"
printf 'GAUDERE_CHAT_HUMAN_TURNS_ADDED=1\n'
printf 'FEDORA_GAUDERE_CHAT_FIRST_PRODUCTION_TURN=PASS\n'