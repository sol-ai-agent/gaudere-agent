#!/bin/sh
set -eu

authorization=${GAUDERE_DIALOGUE_RESPONDER_DEPLOY_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_previous_image=${GAUDERE_EXPECTED_PREVIOUS_IMAGE:-c5a4d33177d1aaf915d82b98dc292fed91847bf88206cc87ffb00c00a39ff418}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
root_task_id=cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19
v2_head_task_id=cognition.local-goose-dialogue.v2:3723223592d31a36116fd30da83a49c23519347313e1fb1d90127dbd4d0da11d
v3_head_task_id=cognition.local-goose-dialogue.v3:3a28c66f9d089bccedbac2e51cd62e949905c6a7ec109423e3e9adefef52b65f
thread_alias=main
consumer_id=sol

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
    printf 'gaudere dialogue responder code-only production deploy: FAIL: %s\n' "$*" >&2
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

responder_sidecar_absent()
{
    [ ! -e "$responder_sidecar" ] && [ ! -L "$responder_sidecar" ]
}

verify_dialogue_state()
{
    [ -f "$thread_sidecar" ] && [ ! -L "$thread_sidecar" ] || fail "dialogue thread sidecar is missing or unsafe"
    [ -f "$completion_sidecar" ] && [ ! -L "$completion_sidecar" ] || fail "dialogue completion sidecar is missing or unsafe"
    [ "$(stat -c '%a' "$thread_sidecar")" = "600" ] || fail "dialogue thread sidecar mode is not 0600"
    [ "$(stat -c '%a' "$completion_sidecar")" = "600" ] || fail "dialogue completion sidecar mode is not 0600"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "dialogue thread sidecar schema is not 2"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "dialogue completion sidecar schema is not 1"

    thread_heads=$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_head;')
    thread_history=$(sqlite3 -readonly "$thread_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_thread_history;')
    thread_revision=$(sqlite3 -readonly "$thread_sidecar" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND root_task_id='$root_task_id' AND head_task_id='$v3_head_task_id';")
    history_zero=$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_history WHERE alias='$thread_alias' AND revision=0 AND root_task_id='$root_task_id' AND head_task_id='$v2_head_task_id';")
    history_one=$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_history WHERE alias='$thread_alias' AND revision=1 AND root_task_id='$root_task_id' AND head_task_id='$v3_head_task_id';")
    [ "$thread_heads" = "1" ] || fail "dialogue preferred head count is not exactly 1"
    [ "$thread_history" = "2" ] || fail "dialogue thread history count is not exactly 2"
    [ "$thread_revision" = "1" ] || fail "dialogue preferred head differs from production revision 1"
    [ "$history_zero" = "1" ] || fail "dialogue thread history revision 0 differs"
    [ "$history_one" = "1" ] || fail "dialogue thread history revision 1 differs"

    completion_events=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')
    completion_materialization=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")
    completion_consumers=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')
    consumer_sequence=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")
    event_zero=$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=1 AND thread_alias='$thread_alias' AND thread_revision=0 AND task_id='$v2_head_task_id';")
    event_one=$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=2 AND thread_alias='$thread_alias' AND thread_revision=1 AND task_id='$v3_head_task_id';")
    [ "$completion_events" = "2" ] || fail "dialogue completion event count is not exactly 2"
    [ "$completion_materialization" = "2" ] || fail "dialogue completion materialization cursor is not 2"
    [ "$completion_consumers" = "1" ] || fail "dialogue completion consumer count is not exactly 1"
    [ "$consumer_sequence" = "1" ] || fail "production sol consumer cursor is not exactly 1"
    [ "$event_zero" = "1" ] || fail "dialogue completion event sequence 1 differs"
    [ "$event_one" = "1" ] || fail "dialogue completion event sequence 2 differs"
}

[ "$authorization" = "AUTHORIZED_LOCAL_GOOSE_DIALOGUE_RESPONDER_CODE_DEPLOY" ] || fail "explicit responder code-deploy authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 install mkdir mktemp mv tar sed grep sleep stat tail tr; do
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

[ "$(service_state)" = "active" ] || fail "production service must be active before deploy"
previous_image=$(running_image)
[ "$previous_image" = "$expected_previous_image" ] || fail "production image differs from expected Stage 9I2/9I3 image"
network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production network is not none"
provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total is $provider_before, expected $expected_provider_total"
cursor_before=$(cycle_cursor)
[ "$cursor_before" = "$expected_cycle_cursor" ] || fail "production Local Goose cycle cursor differs from expected dormant cursor"
cycle_tasks_before=$(task_count cognition.local-goose-cycle.v1)
v1_before=$(task_count cognition.local-goose-dialogue.v1)
v2_before=$(task_count cognition.local-goose-dialogue.v2)
v3_before=$(task_count cognition.local-goose-dialogue.v3)
stimuli_before=$(stimulus_count)
[ "$cycle_tasks_before" = "1" ] || fail "production Local Goose cycle Task count is not exactly 1"
[ "$v1_before" = "1" ] || fail "production V1 dialogue Task count is not exactly 1"
[ "$v2_before" = "2" ] || fail "production V2 dialogue Task count is not exactly 2"
[ "$v3_before" = "1" ] || fail "production V3 dialogue Task count is not exactly 1"
[ "$stimuli_before" = "0" ] || fail "production stimulus ledger is not empty"
verify_dialogue_state
responder_sidecar_absent || fail "production responder sidecar already exists before code-only deploy"

thread_revision_before=$thread_revision
completion_events_before=$completion_events
completion_materialization_before=$completion_materialization
completion_consumers_before=$completion_consumers
consumer_sequence_before=$consumer_sequence

grep -q '^Network=none$' "$target_quadlet" || fail "installed Quadlet lacks Network=none"
grep -q -- '--local-goose-model ' "$target_quadlet" || fail "installed Quadlet lacks Local Goose model"
grep -q -- '--local-goose-model-sha256 ' "$target_quadlet" || fail "installed Quadlet lacks Local Goose model SHA256"
grep -q -- '--control-socket /tmp/gaudere-control.sock' "$target_quadlet" || fail "installed Quadlet lacks control socket"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$target_quadlet" || fail "production profile unexpectedly contains provider authority"

transition_root="$data_home/gaudere/.dialogue-responder-code-deploy"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
previous_quadlet="$workspace/gaudere-agent.container.before"
candidate_quadlet="$workspace/gaudere-agent.container.candidate"
install -m 0600 "$target_quadlet" "$previous_quadlet"

candidate_tag="localhost/gaudere-agent:dialogue-responder-code-$agent_ref"
printf '=== BUILD DIALOGUE RESPONDER CODE-ONLY CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$candidate_tag" sh "$build_script"

provenance_output=$(PODMAN="$podman_command" sh "$provenance_script" "$candidate_tag" "$agent_ref" "$core_ref") || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
candidate_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p' | tail -n 1)
case "$candidate_id" in
    sha256:*) ;;
    *) fail "provenance verifier did not return immutable sha256 image ID" ;;
esac
candidate_normalized=$(printf '%s\n' "$candidate_id" | sed 's/^sha256://')

control_help_status=0
control_help=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs --security-opt=no-new-privileges --cap-drop=all --entrypoint /usr/local/bin/gaudere-control "$candidate_id" --help 2>&1) || control_help_status=$?
[ "$control_help_status" = "2" ] || fail "candidate control usage probe returned unexpected status $control_help_status"
for operation in dialogue-responder-lease-create dialogue-responder-lease-revoke dialogue-responder-lease dialogue-responder-prepare dialogue-responder-dispatch dialogue-responder-intent; do
    printf '%s\n' "$control_help" | grep -q "$operation" || fail "candidate control client lacks $operation"
done
printf '%s\n' "$control_help" | grep -q 'dialogue-feed-next' || fail "candidate control client lost dialogue-feed-next"
printf '%s\n' "$control_help" | grep -q 'dialogue-feed-ack' || fail "candidate control client lost dialogue-feed-ack"
printf '%s\n' "$control_help" | grep -q 'local-thread-v3-send' || fail "candidate control client lost local-thread-v3-send"

python3 - "$target_quadlet" "$candidate_quadlet" "$candidate_id" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
candidate = sys.argv[3]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
indices = [i for i, line in enumerate(lines) if line.startswith("Image=")]
if len(indices) != 1:
    raise SystemExit("installed Quadlet must contain exactly one Image=")
i = indices[0]
ending = "\n" if lines[i].endswith("\n") else ""
lines[i] = f"Image={candidate}{ending}"
destination.write_text("".join(lines), encoding="utf-8")

old = source.read_text(encoding="utf-8").splitlines()
new = destination.read_text(encoding="utf-8").splitlines()
def normalize(items):
    return ["Image=<allowed>" if line.startswith("Image=") else line for line in items]
if normalize(old) != normalize(new):
    raise SystemExit("dialogue responder code deploy attempted a mutation beyond Image=")
PY

backup_archive=""
service_stopped=0
state_mutated=0
profile_mutated=0
committed=0

recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = "1" ]; then
        exit "$status"
    fi
    if [ "$service_stopped" = "0" ] && [ "$state_mutated" = "0" ] && [ "$profile_mutated" = "0" ]; then
        exit "$status"
    fi

    printf 'gaudere dialogue responder code deploy: recovery starting\n' >&2
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

printf '=== STOP / BACKUP / SWITCH IMAGE ONLY ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly"

backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ] || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"

# Any failure after candidate startup may have touched durable state; restore the stopped backup.
state_mutated=1
install -m 0600 "$candidate_quadlet" "$target_quadlet"
profile_mutated=1
"$systemctl_command" --user daemon-reload

printf '=== START PRODUCTION WITH RESPONDER CODE PRESENT BUT DISABLED ===\n'
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "candidate production service did not become active"
[ "$(running_image)" = "$candidate_normalized" ] || fail "running image is not approved candidate"
sleep 2
[ "$(service_state)" = "active" ] || fail "candidate production service did not remain active"

network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "none" ] || fail "production network changed from none"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during responder code deploy"
cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ] || fail "autonomous cycle cursor changed during responder code deploy"
cycle_tasks_after=$(task_count cognition.local-goose-cycle.v1)
v1_after=$(task_count cognition.local-goose-dialogue.v1)
v2_after=$(task_count cognition.local-goose-dialogue.v2)
v3_after=$(task_count cognition.local-goose-dialogue.v3)
stimuli_after=$(stimulus_count)
[ "$cycle_tasks_after" = "$cycle_tasks_before" ] || fail "code-only deploy changed Local Goose cycle Task count"
[ "$v1_after" = "$v1_before" ] || fail "code-only deploy changed V1 dialogue Task count"
[ "$v2_after" = "$v2_before" ] || fail "code-only deploy changed V2 dialogue Task count"
[ "$v3_after" = "$v3_before" ] || fail "code-only deploy changed V3 dialogue Task count"
[ "$stimuli_after" = "$stimuli_before" ] || fail "code-only deploy changed stimulus ledger"

verify_dialogue_state
[ "$thread_revision" = "$thread_revision_before" ] || fail "code-only deploy changed preferred thread revision"
[ "$completion_events" = "$completion_events_before" ] || fail "code-only deploy changed completion event count"
[ "$completion_materialization" = "$completion_materialization_before" ] || fail "code-only deploy changed completion materialization cursor"
[ "$completion_consumers" = "$completion_consumers_before" ] || fail "code-only deploy changed completion consumer count"
[ "$consumer_sequence" = "$consumer_sequence_before" ] || fail "code-only deploy changed sol completion cursor"
responder_sidecar_absent || fail "code-only deploy created responder sidecar"

responder_probe_status=0
responder_probe=$("$podman_command" exec gaudere-agent /usr/local/bin/gaudere-control \
    --socket /tmp/gaudere-control.sock dialogue-responder-lease proof-disabled 2>&1) || responder_probe_status=$?
[ "$responder_probe_status" = "4" ] || fail "disabled responder live probe returned unexpected status $responder_probe_status"
printf '%s\n' "$responder_probe" | grep -q 'dialogue responder capability is not enabled in this service' || fail "candidate responder live probe is not explicitly disabled"
responder_sidecar_absent || fail "disabled responder live probe created responder sidecar"

! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$target_quadlet" || fail "provider authority appeared after deploy"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'CANDIDATE_IMAGE=%s\n' "$candidate_normalized"
printf 'ROLLBACK_IMAGE=%s\n' "$previous_image"
printf 'NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$cursor_after"
printf 'LOCAL_GOOSE_CYCLE_TASKS=%s\n' "$cycle_tasks_after"
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$v1_after"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$v2_after"
printf 'LOCAL_GOOSE_DIALOGUE_V3_TASKS=%s\n' "$v3_after"
printf 'STIMULI=%s\n' "$stimuli_after"
printf 'DIALOGUE_THREAD_REVISION=%s\n' "$thread_revision"
printf 'DIALOGUE_THREAD_HEAD=%s\n' "$v3_head_task_id"
printf 'DIALOGUE_COMPLETION_EVENTS=%s\n' "$completion_events"
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=%s\n' "$completion_materialization"
printf 'DIALOGUE_COMPLETION_CONSUMERS=%s\n' "$completion_consumers"
printf 'DIALOGUE_CONSUMER=%s:%s\n' "$consumer_id" "$consumer_sequence"
printf 'DIALOGUE_NEXT_PENDING_SEQUENCE=2\n'
printf 'RESPONDER_SIDECAR_PRESENT=no\n'
printf 'RESPONDER_LIVE_AUTHORITY=disabled\n'
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_LOCAL_GOOSE_DIALOGUE_RESPONDER_CODE_DEPLOY=PASS\n'
