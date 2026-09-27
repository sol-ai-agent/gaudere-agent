#!/bin/sh
set -eu

authorization=${GAUDERE_FIRST_DIALOGUE_FEED_ACK_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-c5a4d33177d1aaf915d82b98dc292fed91847bf88206cc87ffb00c00a39ff418}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
consumer_id=${GAUDERE_DIALOGUE_FEED_CONSUMER_ID:-sol}
thread_alias="main"
root_task_id="cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19"
v2_head_task_id="cognition.local-goose-dialogue.v2:3723223592d31a36116fd30da83a49c23519347313e1fb1d90127dbd4d0da11d"
v3_head_task_id="cognition.local-goose-dialogue.v3:3a28c66f9d089bccedbac2e51cd62e949905c6a7ec109423e3e9adefef52b65f"
v2_result_sha256="0bbd1857634f2c6502d8700c3e7819f48bce071d7c1c4267146d1308469e88a0"
v3_result_sha256="b4581fe22733187512fad449bcf0f17fc1d1237744b3918b06289693c9df9544"

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
thread_sidecar="$state_directory/local-goose-dialogue-thread.db"
completion_sidecar="$state_directory/local-goose-dialogue-completion.db"
backup_script="$script_directory/backup-state.sh"

fail()
{
    printf 'gaudere first dialogue feed acknowledgement: FAIL: %s\n' "$*" >&2
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
    "$podman_command" exec gaudere-agent \
        /usr/local/bin/gaudere-control \
        --socket /tmp/gaudere-control.sock "$@"
}

verify_static_state()
{
    [ "$(service_state)" = "active" ] || fail "production service is not active"
    [ "$(running_image)" = "$expected_image" ] || fail "production image differs from approved Stage 9I2 image"
    network=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
    [ "$network" = "none" ] || fail "production network is not none"
    [ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs"
    [ "$(cycle_cursor)" = "$expected_cycle_cursor" ] || fail "autonomous cycle cursor differs"
    [ "$(task_count cognition.local-goose-cycle.v1)" = "1" ] || fail "cycle Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v1)" = "1" ] || fail "V1 dialogue Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v2)" = "2" ] || fail "V2 dialogue Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v3)" = "1" ] || fail "V3 dialogue Task count differs"
    [ "$(stimulus_count)" = "0" ] || fail "stimulus ledger is not empty"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "thread schema differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "completion schema differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=1 AND root_task_id='$root_task_id' AND head_task_id='$v3_head_task_id';")" = "1" ] || fail "preferred thread head differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event;")" = "2" ] || fail "completion event count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")" = "2" ] || fail "completion materialization cursor differs"
}

[ "$authorization" = "AUTHORIZED_FIRST_PRODUCTION_DIALOGUE_FEED_ACK" ] || fail "explicit first production dialogue-feed ACK authorization token is required"
[ "$consumer_id" = "sol" ] || fail "first production consumer id must be exactly sol"

for command in "$podman_command" "$systemctl_command" sqlite3 python3 sed grep tar mktemp mkdir mv install stat; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$backup_script"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done
[ "$(stat -c '%a' "$completion_sidecar")" = "600" ] || fail "completion sidecar mode is not 0600"

verify_static_state

python3 - "$completion_sidecar" "$thread_alias" "$root_task_id" "$v2_head_task_id" "$v3_head_task_id" "$v2_result_sha256" "$v3_result_sha256" <<'PY'
import sqlite3
import sys

path, alias, root_id, v2_id, v3_id, v2_sha, v3_sha = sys.argv[1:]
db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
events = db.execute(
    "SELECT sequence,thread_alias,thread_revision,task_id,root_task_id,turn_index,"
    "request_id,speaker_kind,speaker_id,message_kind,result_sha256,response "
    "FROM local_goose_dialogue_completion_event ORDER BY sequence"
).fetchall()
cursors = db.execute(
    "SELECT consumer_id,last_sequence FROM local_goose_dialogue_consumer_cursor ORDER BY consumer_id"
).fetchall()
db.close()
if cursors:
    raise SystemExit(f"consumer cursor already exists before first ACK: {cursors!r}")
if len(events) != 2:
    raise SystemExit(f"expected exactly two completion events, got {len(events)}")
first, second = events
if not (
    first[0] == 1 and first[1] == alias and first[2] == 0
    and first[3] == v2_id and first[4] == root_id and first[5] == 1
    and first[6] == "first-production-dialogue-v2-turn-1"
    and first[7] == "human" and first[8] == "legacy-v2-human"
    and first[9] == "dialogue" and first[10] == v2_sha
    and isinstance(first[11], str) and first[11].strip()
):
    raise SystemExit(f"sequence 1 completion event differs: {first!r}")
if not (
    second[0] == 2 and second[1] == alias and second[2] == 1
    and second[3] == v3_id and second[4] == root_id and second[5] == 2
    and second[6] == "first-production-dialogue-v3-system-observation"
    and second[7] == "system" and second[8] == "sol"
    and second[9] == "observation" and second[10] == v3_sha
    and isinstance(second[11], str) and second[11].strip()
):
    raise SystemExit(f"sequence 2 completion event differs: {second!r}")
PY

printf '=== PRE-EFFECT READ SEQUENCE 1 ===\n'
next1=$(control dialogue-feed-next "$consumer_id") || fail "dialogue-feed-next failed before first ACK"
printf '%s\n' "$next1"
printf '%s\n' "$next1" | grep -q '^pending=true$' || fail "first feed read is not pending"
printf '%s\n' "$next1" | grep -q '^sequence=1$' || fail "first feed read did not return sequence 1"
printf '%s\n' "$next1" | grep -q "^thread_alias=\"$thread_alias\"$" || fail "first feed read thread alias differs"
printf '%s\n' "$next1" | grep -q '^thread_revision=0$' || fail "first feed read revision differs"
printf '%s\n' "$next1" | grep -q "^task_id=\"$v2_head_task_id\"$" || fail "first feed read Task differs"
printf '%s\n' "$next1" | grep -q '^turn_index=1$' || fail "first feed read turn differs"
printf '%s\n' "$next1" | grep -q '^request_id="first-production-dialogue-v2-turn-1"$' || fail "first feed read request id differs"
printf '%s\n' "$next1" | grep -q '^speaker_kind="human"$' || fail "first feed read speaker kind differs"
printf '%s\n' "$next1" | grep -q '^speaker_id="legacy-v2-human"$' || fail "first feed read speaker id differs"
printf '%s\n' "$next1" | grep -q '^message_kind="dialogue"$' || fail "first feed read message kind differs"
printf '%s\n' "$next1" | grep -q "^result_sha256=$v2_result_sha256$" || fail "first feed read result SHA differs"

data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
transition_root="$data_home/gaudere/.dialogue-feed-first-ack"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")

printf '=== STOP / BACKUP BEFORE FIRST ACK ===\n'
"$systemctl_command" --user stop "$service_name"
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly before backup"
backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ] || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "production service did not restart after backup"

mutated=0
committed=0

recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = "1" ] || [ "$mutated" = "0" ]; then
        exit "$status"
    fi
    printf 'gaudere first dialogue feed acknowledgement: recovery starting\n' >&2
    "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
    failed_state="$workspace/failed-state"
    if [ ! -e "$failed_state" ]; then
        mv "$state_directory" "$failed_state" || true
        mkdir -p -m 0700 "$state_directory" || true
        tar -xzf "$backup_archive" -C "$state_directory" || true
    fi
    "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
    printf 'recovery_service=%s\n' "$(service_state)" >&2
    printf 'recovery_image=%s\n' "$(running_image)" >&2
    printf 'recovery_workspace=%s\n' "$workspace" >&2
    exit "$status"
}
trap recover EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '=== ACK EXACTLY SEQUENCE 1 ===\n'
ack1=$(control dialogue-feed-ack "$consumer_id" 1) || fail "first production dialogue-feed ACK failed"
mutated=1
printf '%s\n' "$ack1"
printf '%s\n' "$ack1" | grep -q "^consumer_id=\"$consumer_id\"$" || fail "ACK consumer id differs"
printf '%s\n' "$ack1" | grep -q '^result=accepted$' || fail "first production ACK was not accepted"
printf '%s\n' "$ack1" | grep -q '^last_sequence=1$' || fail "first production ACK did not advance to sequence 1"

[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;")" = "1" ] || fail "consumer cursor row count is not exactly 1"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "consumer cursor did not persist sequence 1"

printf '=== READ NEXT WITHOUT ACK ===\n'
next2=$(control dialogue-feed-next "$consumer_id") || fail "dialogue-feed-next failed after first ACK"
printf '%s\n' "$next2"
printf '%s\n' "$next2" | grep -q '^pending=true$' || fail "second feed event is not pending"
printf '%s\n' "$next2" | grep -q '^sequence=2$' || fail "second feed read did not return sequence 2"
printf '%s\n' "$next2" | grep -q "^thread_alias=\"$thread_alias\"$" || fail "second feed read thread alias differs"
printf '%s\n' "$next2" | grep -q '^thread_revision=1$' || fail "second feed read revision differs"
printf '%s\n' "$next2" | grep -q "^task_id=\"$v3_head_task_id\"$" || fail "second feed read Task differs"
printf '%s\n' "$next2" | grep -q '^turn_index=2$' || fail "second feed read turn differs"
printf '%s\n' "$next2" | grep -q '^request_id="first-production-dialogue-v3-system-observation"$' || fail "second feed read request id differs"
printf '%s\n' "$next2" | grep -q '^speaker_kind="system"$' || fail "second feed read speaker kind differs"
printf '%s\n' "$next2" | grep -q '^speaker_id="sol"$' || fail "second feed read speaker id differs"
printf '%s\n' "$next2" | grep -q '^message_kind="observation"$' || fail "second feed read message kind differs"
printf '%s\n' "$next2" | grep -q "^result_sha256=$v3_result_sha256$" || fail "second feed read result SHA differs"

verify_static_state
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;")" = "1" ] || fail "consumer cursor row count changed unexpectedly"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "consumer cursor changed beyond sequence 1"

committed=1
trap - EXIT HUP INT TERM

printf 'CONSUMER_ID=%s\n' "$consumer_id"
printf 'ACKED_SEQUENCE=1\n'
printf 'NEXT_PENDING_SEQUENCE=2\n'
printf 'DIALOGUE_COMPLETION_EVENTS=2\n'
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=2\n'
printf 'DIALOGUE_COMPLETION_CONSUMERS=1\n'
printf 'PRODUCTION_IMAGE=%s\n' "$(running_image)"
printf 'NETWORK=none\n'
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'CYCLE_CURSOR=%s\n' "$(cycle_cursor)"
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FIRST_PRODUCTION_DIALOGUE_FEED_ACK=PASS\n'
