#!/bin/sh
set -eu

authorization=${GAUDERE_FIRST_DIALOGUE_RESPONDER_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_previous_image=${GAUDERE_EXPECTED_PREVIOUS_IMAGE:-654e1ae541a875f5f9779aa98df46f450dea56907e2efd15d791d4109afb8c75}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
root_task_id=cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19
v2_head_task_id=cognition.local-goose-dialogue.v2:3723223592d31a36116fd30da83a49c23519347313e1fb1d90127dbd4d0da11d
v3_head_task_id=cognition.local-goose-dialogue.v3:3a28c66f9d089bccedbac2e51cd62e949905c6a7ec109423e3e9adefef52b65f
thread_alias=main
manual_consumer_id=sol
responder_consumer_id=system-responder-v1
lease_id=first-production-responder-v1
speaker_id=sol
message_kind=intervention
purpose='First bounded production system responder turn for Stage 9J6'
message="Merci. Je confirme que ce message provient du responder système borné de Gaudere. Cette première intervention de production est limitée à un seul tour et n'active ni provider externe ni cycle autonome. Peux-tu confirmer que tu reçois ce tour système ?"
lease_ttl_ms=600000
intent_ttl_ms=300000
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}

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
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
backup_script="$script_directory/backup-state.sh"
build_script="$script_directory/build-image.sh"
provenance_script="$script_directory/verify-image-provenance.sh"

fail()
{
    printf 'gaudere first production dialogue responder v1: FAIL: %s\n' "$*" >&2
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

control()
{
    "$podman_command" exec gaudere-agent /usr/local/bin/gaudere-control --socket /tmp/gaudere-control.sock "$@"
}

verify_pre_activation_state()
{
    [ -f "$thread_sidecar" ] && [ ! -L "$thread_sidecar" ] || fail "dialogue thread sidecar is missing or unsafe"
    [ -f "$completion_sidecar" ] && [ ! -L "$completion_sidecar" ] || fail "dialogue completion sidecar is missing or unsafe"
    [ "$(stat -c '%a' "$thread_sidecar")" = "600" ] || fail "dialogue thread sidecar mode is not 0600"
    [ "$(stat -c '%a' "$completion_sidecar")" = "600" ] || fail "dialogue completion sidecar mode is not 0600"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "dialogue thread sidecar schema is not 2"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "dialogue completion sidecar schema is not 1"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_head;')" = "1" ] || fail "preferred head count differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_history;')" = "2" ] || fail "preferred history count differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=1 AND root_task_id='$root_task_id' AND head_task_id='$v3_head_task_id';")" = "1" ] || fail "preferred head is not canonical revision 1"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')" = "2" ] || fail "completion event count is not 2"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")" = "2" ] || fail "completion materialization cursor is not 2"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')" = "1" ] || fail "completion consumer count is not 1"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")" = "1" ] || fail "manual sol cursor is not 1"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "0" ] || fail "responder consumer already exists"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=1 AND task_id='$v2_head_task_id';")" = "1" ] || fail "sequence 1 differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=2 AND task_id='$v3_head_task_id';")" = "1" ] || fail "sequence 2 differs"
    [ ! -e "$responder_sidecar" ] && [ ! -L "$responder_sidecar" ] || fail "responder sidecar already exists before first activation"
}

[ "$authorization" = "AUTHORIZED_FIRST_PRODUCTION_DIALOGUE_RESPONDER_V1" ] || fail "explicit first responder production authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 install mkdir mktemp mv tar sed grep sleep stat tr; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
for file in "$backup_script" "$build_script" "$provenance_script" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$target_quadlet"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
[ "$(service_state)" = "active" ] || fail "production service must be active before activation"
previous_image=$(running_image)
[ "$previous_image" = "$expected_previous_image" ] || fail "production image differs from Stage 9J5 image"
network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production network is not none"
provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total differs"
cursor_before=$(cycle_cursor)
[ "$cursor_before" = "$expected_cycle_cursor" ] || fail "autonomous cycle cursor differs"
cycle_tasks_before=$(task_count cognition.local-goose-cycle.v1)
v1_before=$(task_count cognition.local-goose-dialogue.v1)
v2_before=$(task_count cognition.local-goose-dialogue.v2)
v3_before=$(task_count cognition.local-goose-dialogue.v3)
stimuli_before=$(stimulus_count)
[ "$cycle_tasks_before" = "1" ] || fail "cycle Task count differs"
[ "$v1_before" = "1" ] || fail "V1 Task count differs"
[ "$v2_before" = "2" ] || fail "V2 Task count differs"
[ "$v3_before" = "1" ] || fail "V3 Task count differs"
[ "$stimuli_before" = "0" ] || fail "stimulus ledger is not empty"
verify_pre_activation_state

grep -q '^Network=none$' "$target_quadlet" || fail "installed Quadlet lacks Network=none"
grep -q -- '--local-goose-model ' "$target_quadlet" || fail "installed Quadlet lacks Local Goose model"
grep -q -- '--local-goose-model-sha256 ' "$target_quadlet" || fail "installed Quadlet lacks Local Goose model SHA256"
grep -q -- '--control-socket /tmp/gaudere-control.sock' "$target_quadlet" || fail "installed Quadlet lacks control socket"
! grep -q -- '--local-goose-dialogue-responder-' "$target_quadlet" || fail "installed Quadlet already contains responder activation"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$target_quadlet" || fail "production profile unexpectedly contains provider authority"

transition_root="$data_home/gaudere/.dialogue-responder-first-production"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
previous_quadlet="$workspace/gaudere-agent.container.before"
candidate_quadlet="$workspace/gaudere-agent.container.candidate"
install -m 0600 "$target_quadlet" "$previous_quadlet"

candidate_tag="localhost/gaudere-agent:first-dialogue-responder-$agent_ref"
printf '=== BUILD FIRST PRODUCTION RESPONDER CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$candidate_tag" sh "$build_script"
provenance_output=$(PODMAN="$podman_command" sh "$provenance_script" "$candidate_tag" "$agent_ref" "$core_ref") || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
candidate_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p' | tail -n 1)
case "$candidate_id" in sha256:*) ;; *) fail "provenance verifier did not return immutable image ID" ;; esac
candidate_normalized=$(printf '%s\n' "$candidate_id" | sed 's/^sha256://')

agent_help=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs --security-opt=no-new-privileges --cap-drop=all --entrypoint /usr/local/bin/gaudere-agent "$candidate_id" --help 2>&1) || fail "candidate agent help probe failed"
printf '%s\n' "$agent_help" | grep -q -- '--local-goose-dialogue-responder-sidecar PATH' || fail "candidate lacks responder sidecar service option"
printf '%s\n' "$agent_help" | grep -q -- '--local-goose-dialogue-responder-consumer-id ID' || fail "candidate lacks responder consumer service option"

python3 - "$target_quadlet" "$candidate_quadlet" "$candidate_id" "$responder_consumer_id" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1])
destination = Path(sys.argv[2])
candidate = sys.argv[3]
consumer = sys.argv[4]
old = source.read_text(encoding="utf-8").splitlines(keepends=True)
image_indices = [i for i,l in enumerate(old) if l.startswith("Image=")]
exec_indices = [i for i,l in enumerate(old) if l.startswith("Exec=")]
if len(image_indices) != 1 or len(exec_indices) != 1:
    raise SystemExit("installed Quadlet must contain exactly one Image= and Exec=")
if any("--local-goose-dialogue-responder-" in l for l in old):
    raise SystemExit("installed Quadlet already contains responder options")
new = list(old)
i = image_indices[0]
ending = "\n" if new[i].endswith("\n") else ""
new[i] = f"Image={candidate}{ending}"
e = exec_indices[0]
ending = "\n" if new[e].endswith("\n") else ""
base = new[e][:-1] if ending else new[e]
new[e] = (
    base
    + " --local-goose-dialogue-responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db"
    + f" --local-goose-dialogue-responder-consumer-id {consumer}"
    + ending
)
destination.write_text("".join(new), encoding="utf-8")
PY

grep -q -- '--local-goose-dialogue-responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db' "$candidate_quadlet" || fail "candidate Quadlet lacks responder sidecar option"
grep -q -- "--local-goose-dialogue-responder-consumer-id $responder_consumer_id" "$candidate_quadlet" || fail "candidate Quadlet lacks responder consumer id"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$candidate_quadlet" || fail "candidate profile unexpectedly contains provider authority"

backup_archive=""
service_stopped=0
state_mutated=0
profile_mutated=0
committed=0
recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = "1" ]; then exit "$status"; fi
    if [ "$service_stopped" = "0" ] && [ "$state_mutated" = "0" ] && [ "$profile_mutated" = "0" ]; then exit "$status"; fi
    printf 'gaudere first production dialogue responder v1: recovery starting\n' >&2
    "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
    if [ "$profile_mutated" = "1" ]; then
        install -m 0600 "$previous_quadlet" "$target_quadlet" || true
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
    fi
    if [ "$state_mutated" = "1" ] && [ -n "$backup_archive" ] && [ -f "$backup_archive" ]; then
        failed_state="$workspace/failed-state"
        if [ ! -e "$failed_state" ]; then
            mv "$state_directory" "$failed_state" || true
            mkdir -p -m 0700 "$state_directory" || true
            tar -xzf "$backup_archive" -C "$state_directory" || true
        fi
    fi
    "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
    printf 'recovery_service=%s\n' "$(service_state)" >&2
    printf 'recovery_image=%s\n' "$(running_image)" >&2
    printf 'recovery_provider_total=%s\n' "$(provider_total 2>/dev/null || true)" >&2
    printf 'recovery_workspace=%s\n' "$workspace" >&2
    exit "$status"
}
trap recover EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '=== STOP / BACKUP / ENABLE EXPLICIT RESPONDER WIRING ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly"
backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ] || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"
state_mutated=1
install -m 0600 "$candidate_quadlet" "$target_quadlet"
profile_mutated=1
"$systemctl_command" --user daemon-reload
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "responder-wired service did not become active"
[ "$(running_image)" = "$candidate_normalized" ] || fail "running image is not candidate"
sleep 2
[ "$(service_state)" = "active" ] || fail "responder-wired service did not remain active"

[ -f "$responder_sidecar" ] && [ ! -L "$responder_sidecar" ] || fail "responder sidecar was not created safely"
[ "$(stat -c '%a' "$responder_sidecar")" = "600" ] || fail "responder sidecar mode is not 0600"
[ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "responder sidecar schema is not 1"
[ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;')" = "0" ] || fail "responder sidecar unexpectedly contains a lease"
[ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent;')" = "0" ] || fail "responder sidecar unexpectedly contains an intent"

network_wired=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_wired" = "none" ] || fail "network changed after responder wiring"
[ "$(provider_total)" = "$provider_before" ] || fail "provider total changed after responder wiring"
[ "$(cycle_cursor)" = "$cursor_before" ] || fail "cycle changed after responder wiring"
[ "$(task_count cognition.local-goose-dialogue.v3)" = "$v3_before" ] || fail "V3 Task count changed before responder dispatch"

printf '=== EXPLICIT RESPONDER CONSUMER MIGRATION ===\n'
first_pending=$(control dialogue-feed-next "$responder_consumer_id") || fail "responder initial feed read failed"
printf '%s\n' "$first_pending"
printf '%s\n' "$first_pending" | grep -q '^sequence=1$' || fail "fresh responder consumer did not start at sequence 1"
seed_ack=$(control dialogue-feed-ack "$responder_consumer_id" 1) || fail "responder consumer migration ACK failed"
printf '%s\n' "$seed_ack"
printf '%s\n' "$seed_ack" | grep -Eq '^result=(accepted|duplicate)$' || fail "responder migration ACK was not accepted"
printf '%s\n' "$seed_ack" | grep -q '^last_sequence=1$' || fail "responder migration cursor is not 1"
second_pending=$(control dialogue-feed-next "$responder_consumer_id") || fail "responder sequence 2 read failed"
printf '%s\n' "$second_pending"
printf '%s\n' "$second_pending" | grep -q '^sequence=2$' || fail "responder consumer next pending is not sequence 2"
printf '%s\n' "$second_pending" | grep -q "^task_id=\"$v3_head_task_id\"$" || fail "responder sequence 2 task differs"
sol_pending=$(control dialogue-feed-next "$manual_consumer_id") || fail "sol feed verification failed"
printf '%s\n' "$sol_pending" | grep -q '^sequence=2$' || fail "sol cursor changed during responder migration"

printf '=== CREATE ONE-TURN RESPONDER LEASE ===\n'
lease_output=$(control dialogue-responder-lease-create "$lease_id" "$thread_alias" "$speaker_id" "$message_kind" 1 "$lease_ttl_ms" 0 "$purpose") || fail "responder lease creation failed"
printf '%s\n' "$lease_output"
printf '%s\n' "$lease_output" | grep -Eq '^result=(accepted|duplicate)$' || fail "responder lease was not accepted"
printf '%s\n' "$lease_output" | grep -q '^state=active$' || fail "responder lease is not active"
printf '%s\n' "$lease_output" | grep -q '^turns_committed=0$' || fail "responder lease already consumed a turn"

printf '=== PREPARE EXACT RESPONDER INTENT FOR SEQUENCE 2 ===\n'
prepare_output=$(control dialogue-responder-prepare "$lease_id" 2 "$message_kind" "$intent_ttl_ms" "$message") || fail "responder intent preparation failed"
printf '%s\n' "$prepare_output"
printf '%s\n' "$prepare_output" | grep -Eq '^result=(accepted|duplicate)$' || fail "responder intent was not accepted"
printf '%s\n' "$prepare_output" | grep -q '^consumer_last_sequence=1$' || fail "responder cursor advanced before dispatch"
intent_id=$(printf '%s\n' "$prepare_output" | sed -n 's/^intent_id="\(.*\)"$/\1/p' | head -n 1)
responder_request_id=$(printf '%s\n' "$prepare_output" | sed -n 's/^request_id="\(.*\)"$/\1/p' | head -n 1)
[ -n "$intent_id" ] || fail "could not resolve responder intent id"
[ -n "$responder_request_id" ] || fail "could not resolve responder request id"
printf '%s\n' "$prepare_output" | grep -q '^state=prepared

printf '=== DISPATCH EXACT ONE-TURN RESPONDER INTENT ===\n'
dispatch_output=$(control dialogue-responder-dispatch "$intent_id") || fail "responder intent dispatch failed"
printf '%s\n' "$dispatch_output"
printf '%s\n' "$dispatch_output" | grep -Eq '^result=(accepted|duplicate)$' || fail "responder dispatch was not accepted"
printf '%s\n' "$dispatch_output" | grep -q '^consumer_last_sequence=2$' || fail "responder cursor did not advance to 2 after dispatch"
printf '%s\n' "$dispatch_output" | grep -q '^state=completed$' || fail "responder intent did not complete"
printf '%s\n' "$dispatch_output" | grep -q '^revision=2$' || fail "preferred thread did not advance to revision 2"
task_id=$(printf '%s\n' "$dispatch_output" | sed -n 's/^id="\(.*\)"$/\1/p' | head -n 1)
[ -n "$task_id" ] || fail "could not resolve responder successor Task id"

attempt=0
status=""
task_output=""
while [ "$attempt" -lt 180 ]; do
    attempt=$((attempt + 1))
    task_output=$(control task "$task_id") || fail "responder successor Task inspection failed"
    status=$(printf '%s\n' "$task_output" | sed -n 's/^status=//p' | head -n 1)
    case "$status" in succeeded|failed|cancelled|manual_review) break ;; esac
    sleep 1
done
[ "$status" = "succeeded" ] || {
    printf '%s\n' "$task_output" >&2
    fail "responder successor Task did not succeed; status=$status"
}

feed_attempt=0
completion_events=0
completion_materialization=0
while [ "$feed_attempt" -lt 30 ]; do
    feed_attempt=$((feed_attempt + 1))
    completion_events=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')
    completion_materialization=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")
    if [ "$completion_events" = "3" ] && [ "$completion_materialization" = "3" ]; then break; fi
    sleep 1
done
[ "$completion_events" = "3" ] || fail "responder successor completion event was not materialized"
[ "$completion_materialization" = "3" ] || fail "completion materialization cursor did not advance to 3"

python3 - \
    "$state_database" "$completion_sidecar" "$task_id" "$root_task_id" \
    "$v3_head_task_id" "$responder_request_id" "$speaker_id" \
    "$message_kind" "$message" "$model_sha256" <<'PY'
import hashlib
import json
import sqlite3
import sys

(
    state_path,
    completion_path,
    task_id,
    root_id,
    predecessor_id,
    request_id,
    speaker_id,
    message_kind,
    message,
    model_sha,
) = sys.argv[1:]

state = sqlite3.connect(f"file:{state_path}?mode=ro", uri=True)
predecessor = state.execute(
    "SELECT result_output FROM tasks WHERE id=? "
    "AND kind='cognition.local-goose-dialogue.v3' AND status=3",
    (predecessor_id,),
).fetchone()
task = state.execute(
    "SELECT input,status,attempts_started,result_content_type,result_output,"
    "COALESCE(result_failure_code,''),COALESCE(result_failure_message,'') "
    "FROM tasks WHERE id=? AND kind='cognition.local-goose-dialogue.v3'",
    (task_id,),
).fetchone()
state.close()

if predecessor is None or predecessor[0] is None:
    raise SystemExit("canonical responder predecessor result is missing")
if task is None:
    raise SystemExit("responder successor Task is missing")

raw_input, status, attempts, result_type, raw_result, failure_code, failure_message = task
if status != 3 or attempts != 1:
    raise SystemExit(
        f"responder successor is not one canonical success: "
        f"status={status} attempts={attempts}"
    )
if result_type != "application/vnd.gaudere.local-goose-dialogue-v3-response+json":
    raise SystemExit("responder successor result content type differs")
if failure_code or failure_message or raw_result is None:
    raise SystemExit("responder successor contains failure evidence")

inp = json.loads(raw_input)
predecessor_sha = hashlib.sha256(predecessor[0].encode("utf-8")).hexdigest()
expected = {
    "schema": "gaudere.cognition.local-goose-dialogue.v3",
    "request_id": request_id,
    "speaker_kind": "system",
    "speaker_id": speaker_id,
    "message_kind": message_kind,
    "message": message,
    "model_sha256": model_sha,
    "turn_index": 3,
    "root_task_id": root_id,
    "predecessor_task_id": predecessor_id,
    "predecessor_result_sha256": predecessor_sha,
}
for key, value in expected.items():
    if inp.get(key) != value:
        raise SystemExit(
            f"responder successor input differs for {key}: "
            f"{inp.get(key)!r} != {value!r}"
        )
expected_task_id = (
    "cognition.local-goose-dialogue.v3:"
    + hashlib.sha256(raw_input.encode("utf-8")).hexdigest()
)
if task_id != expected_task_id:
    raise SystemExit("responder successor Task id differs from canonical input hash")

out = json.loads(raw_result)
for key, value in {
    "schema": "gaudere.cognition.local-goose-dialogue-v3-response.v1",
    "request_id": request_id,
    "speaker_kind": "system",
    "speaker_id": speaker_id,
    "message_kind": message_kind,
    "model_sha256": model_sha,
    "turn_index": 3,
    "root_task_id": root_id,
    "predecessor_task_id": predecessor_id,
    "predecessor_result_sha256": predecessor_sha,
}.items():
    if out.get(key) != value:
        raise SystemExit(
            f"responder successor result differs for {key}: "
            f"{out.get(key)!r} != {value!r}"
        )
if not isinstance(out.get("response"), str) or not out["response"].strip():
    raise SystemExit("responder successor response is empty")

result_sha = hashlib.sha256(raw_result.encode("utf-8")).hexdigest()
completion = sqlite3.connect(f"file:{completion_path}?mode=ro", uri=True)
event = completion.execute(
    "SELECT thread_alias,thread_revision,task_id,result_sha256 "
    "FROM local_goose_dialogue_completion_event WHERE sequence=3"
).fetchone()
completion.close()
if event != ("main", 2, task_id, result_sha):
    raise SystemExit(f"completion sequence 3 differs: {event!r}")
print("RESPONDER_RESPONSE_JSON=" + json.dumps(out["response"], ensure_ascii=False))
PY

next_responder=$(control dialogue-feed-next "$responder_consumer_id") || fail "responder post-dispatch feed read failed"
printf '%s\n' "$next_responder"
printf '%s\n' "$next_responder" | grep -q '^sequence=3$' || fail "sequence 3 is not pending for responder consumer"
sol_pending_after=$(control dialogue-feed-next "$manual_consumer_id") || fail "sol post-dispatch feed read failed"
printf '%s\n' "$sol_pending_after" | grep -q '^sequence=2$' || fail "sol cursor changed during responder activation"

lease_final=$(control dialogue-responder-lease "$lease_id") || fail "final responder lease inspection failed"
printf '%s\n' "$lease_final"
printf '%s\n' "$lease_final" | grep -q '^state=exhausted$' || fail "one-turn responder lease is not exhausted"
printf '%s\n' "$lease_final" | grep -q '^turns_committed=1$' || fail "one-turn responder lease accounting differs"

intent_final=$(control dialogue-responder-intent "$intent_id") || fail "final responder intent inspection failed"
printf '%s\n' "$intent_final" | grep -q '^state=completed$' || fail "responder intent final state is not completed"

[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")" = "1" ] || fail "sol durable cursor changed"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "2" ] || fail "responder durable cursor is not 2"
[ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')" = "2" ] || fail "completion consumer count is not 2"
[ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_id' AND state=1 AND turns_committed=1;")" = "1" ] || fail "responder durable exhausted lease differs"
[ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE intent_id='$intent_id' AND state=2;")" = "1" ] || fail "responder durable completed intent differs"
[ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=2 AND head_task_id='$task_id';")" = "1" ] || fail "preferred durable head is not responder successor revision 2"
[ "$(task_count cognition.local-goose-dialogue.v3)" = "2" ] || fail "production V3 Task count is not exactly 2 after responder turn"
[ "$(provider_total)" = "$provider_before" ] || fail "provider total changed during responder activation"
[ "$(cycle_cursor)" = "$cursor_before" ] || fail "autonomous cycle changed during responder activation"
[ "$(stimulus_count)" = "$stimuli_before" ] || fail "stimulus ledger changed during responder activation"
network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "none" ] || fail "network changed during responder activation"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'CANDIDATE_IMAGE=%s\n' "$candidate_normalized"
printf 'ROLLBACK_IMAGE=%s\n' "$previous_image"
printf 'NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'CYCLE_CURSOR=%s\n' "$(cycle_cursor)"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$(task_count cognition.local-goose-cycle.v1)"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v1)"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v2)"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v3)"
printf 'STIMULI=%s\n' "$(stimulus_count)"
printf 'DIALOGUE_THREAD_REVISION=2\n'
printf 'DIALOGUE_THREAD_HEAD=%s\n' "$task_id"
printf 'DIALOGUE_COMPLETION_EVENTS=3\n'
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=3\n'
printf 'DIALOGUE_COMPLETION_CONSUMERS=2\n'
printf 'DIALOGUE_CONSUMER_SOL=sol:1\n'
printf 'DIALOGUE_CONSUMER_RESPONDER=%s:2\n' "$responder_consumer_id"
printf 'DIALOGUE_RESPONDER_NEXT_PENDING_SEQUENCE=3\n'
printf 'RESPONDER_LEASE=%s:exhausted:1/1\n' "$lease_id"
printf 'RESPONDER_INTENT=%s:completed\n' "$intent_id"
printf 'RESPONDER_REQUEST_ID=%s\n' "$responder_request_id"
printf 'RESPONDER_TASK=%s\n' "$task_id"
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FIRST_PRODUCTION_DIALOGUE_RESPONDER_V1=PASS\n' || fail "responder intent is not prepared"

printf '=== DISPATCH EXACT ONE-TURN RESPONDER INTENT ===\n'
dispatch_output=$(control dialogue-responder-dispatch "$intent_id") || fail "responder intent dispatch failed"
printf '%s\n' "$dispatch_output"
printf '%s\n' "$dispatch_output" | grep -Eq '^result=(accepted|duplicate)$' || fail "responder dispatch was not accepted"
printf '%s\n' "$dispatch_output" | grep -q '^consumer_last_sequence=2$' || fail "responder cursor did not advance to 2 after dispatch"
printf '%s\n' "$dispatch_output" | grep -q '^state=completed$' || fail "responder intent did not complete"
printf '%s\n' "$dispatch_output" | grep -q '^revision=2$' || fail "preferred thread did not advance to revision 2"
task_id=$(printf '%s\n' "$dispatch_output" | sed -n 's/^id="\(.*\)"$/\1/p' | head -n 1)
[ -n "$task_id" ] || fail "could not resolve responder successor Task id"

attempt=0
status=""
task_output=""
while [ "$attempt" -lt 180 ]; do
    attempt=$((attempt + 1))
    task_output=$(control task "$task_id") || fail "responder successor Task inspection failed"
    status=$(printf '%s\n' "$task_output" | sed -n 's/^status=//p' | head -n 1)
    case "$status" in succeeded|failed|cancelled|manual_review) break ;; esac
    sleep 1
done
[ "$status" = "succeeded" ] || {
    printf '%s\n' "$task_output" >&2
    fail "responder successor Task did not succeed; status=$status"
}

feed_attempt=0
completion_events=0
completion_materialization=0
while [ "$feed_attempt" -lt 30 ]; do
    feed_attempt=$((feed_attempt + 1))
    completion_events=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')
    completion_materialization=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")
    if [ "$completion_events" = "3" ] && [ "$completion_materialization" = "3" ]; then break; fi
    sleep 1
done
[ "$completion_events" = "3" ] || fail "responder successor completion event was not materialized"
[ "$completion_materialization" = "3" ] || fail "completion materialization cursor did not advance to 3"

next_responder=$(control dialogue-feed-next "$responder_consumer_id") || fail "responder post-dispatch feed read failed"
printf '%s\n' "$next_responder"
printf '%s\n' "$next_responder" | grep -q '^sequence=3$' || fail "sequence 3 is not pending for responder consumer"
sol_pending_after=$(control dialogue-feed-next "$manual_consumer_id") || fail "sol post-dispatch feed read failed"
printf '%s\n' "$sol_pending_after" | grep -q '^sequence=2$' || fail "sol cursor changed during responder activation"

lease_final=$(control dialogue-responder-lease "$lease_id") || fail "final responder lease inspection failed"
printf '%s\n' "$lease_final"
printf '%s\n' "$lease_final" | grep -q '^state=exhausted$' || fail "one-turn responder lease is not exhausted"
printf '%s\n' "$lease_final" | grep -q '^turns_committed=1$' || fail "one-turn responder lease accounting differs"

intent_final=$(control dialogue-responder-intent "$intent_id") || fail "final responder intent inspection failed"
printf '%s\n' "$intent_final" | grep -q '^state=completed$' || fail "responder intent final state is not completed"

[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")" = "1" ] || fail "sol durable cursor changed"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "2" ] || fail "responder durable cursor is not 2"
[ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')" = "2" ] || fail "completion consumer count is not 2"
[ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_id' AND state=1 AND turns_committed=1;")" = "1" ] || fail "responder durable exhausted lease differs"
[ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE intent_id='$intent_id' AND state=2;")" = "1" ] || fail "responder durable completed intent differs"
[ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=2 AND head_task_id='$task_id';")" = "1" ] || fail "preferred durable head is not responder successor revision 2"
[ "$(task_count cognition.local-goose-dialogue.v3)" = "2" ] || fail "production V3 Task count is not exactly 2 after responder turn"
[ "$(provider_total)" = "$provider_before" ] || fail "provider total changed during responder activation"
[ "$(cycle_cursor)" = "$cursor_before" ] || fail "autonomous cycle changed during responder activation"
[ "$(stimulus_count)" = "$stimuli_before" ] || fail "stimulus ledger changed during responder activation"
network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "none" ] || fail "network changed during responder activation"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'CANDIDATE_IMAGE=%s\n' "$candidate_normalized"
printf 'ROLLBACK_IMAGE=%s\n' "$previous_image"
printf 'NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'CYCLE_CURSOR=%s\n' "$(cycle_cursor)"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$(task_count cognition.local-goose-cycle.v1)"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v1)"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v2)"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$(task_count cognition.local-goose-dialogue.v3)"
printf 'STIMULI=%s\n' "$(stimulus_count)"
printf 'DIALOGUE_THREAD_REVISION=2\n'
printf 'DIALOGUE_THREAD_HEAD=%s\n' "$task_id"
printf 'DIALOGUE_COMPLETION_EVENTS=3\n'
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=3\n'
printf 'DIALOGUE_COMPLETION_CONSUMERS=2\n'
printf 'DIALOGUE_CONSUMER_SOL=sol:1\n'
printf 'DIALOGUE_CONSUMER_RESPONDER=%s:2\n' "$responder_consumer_id"
printf 'DIALOGUE_RESPONDER_NEXT_PENDING_SEQUENCE=3\n'
printf 'RESPONDER_LEASE=%s:exhausted:1/1\n' "$lease_id"
printf 'RESPONDER_INTENT=%s:completed\n' "$intent_id"
printf 'RESPONDER_TASK=%s\n' "$task_id"
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FIRST_PRODUCTION_DIALOGUE_RESPONDER_V1=PASS\n'