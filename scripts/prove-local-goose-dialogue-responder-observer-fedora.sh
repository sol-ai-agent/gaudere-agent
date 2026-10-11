#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
thread_alias=${GAUDERE_DIALOGUE_THREAD_ALIAS:-main}
consumer_id=${GAUDERE_DIALOGUE_RESPONDER_CONSUMER_ID:-system-responder-v1}

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
proof_root="$data_home/gaudere/.dialogue-responder-observer-isolated-proof"
workspace=""
proof_builder_image=""
proof_ok=0

fail()
{
    printf 'gaudere dialogue responder observer isolated proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ -n "$proof_builder_image" ]; then
        "$podman_command" image rm "$proof_builder_image" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'gaudere dialogue responder observer isolated proof: diagnostic directory preserved: %s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sed grep awk cut stat cp mkdir mktemp rm id sha256sum chmod basename; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")

case "$agent_ref" in
    *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal Git SHA" ;;
esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"
case "$core_ref" in
    *[!0-9a-f]*|'') fail "gaudere.ref is not one lowercase hexadecimal Git SHA" ;;
esac
[ "${#core_ref}" -eq 40 ] || fail "gaudere.ref is not 40 characters"

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

task_shape()
{
    sqlite3 -readonly "$state_database" "SELECT kind||'='||COUNT(*) FROM tasks GROUP BY kind ORDER BY kind;"
}

production_signature()
{
    printf '%s\n' \
        "service=$(service_state)" \
        "image=$(running_image)" \
        "network=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" \
        "provider=$(provider_total)" \
        "cycle=$(cycle_cursor)" \
        "stimuli=$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" \
        "tasks=$(task_shape | sha256sum | awk '{print $1}')" \
        "thread=$(sqlite3 -readonly "$thread_sidecar" "SELECT revision||'|'||root_task_id||'|'||head_task_id FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" \
        "completion_events=$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')" \
        "materialization=$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")" \
        "consumer=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" \
        "completion_dump=$(sqlite3 -readonly "$completion_sidecar" '.dump' | sha256sum | awk '{print $1}')" \
        "responder_schema=$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" \
        "responder_dump=$(sqlite3 -readonly "$responder_sidecar" '.dump' | sha256sum | awk '{print $1}')"
}

verify_checkpoint()
{
    [ "$(service_state)" = "active" ] || fail "production service is not active"
    [ "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" = "none" ] || fail "production network is not none"
    [ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs"
    [ "$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] || fail "stimulus ledger is not empty"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "thread sidecar schema differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "completion sidecar schema differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "production responder sidecar is not schema v1"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "2" ] || fail "responder consumer cursor is not the post-9K5 value 2"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT speaker_kind FROM local_goose_dialogue_completion_event WHERE sequence=3;")" = "system" ] || fail "sequence 3 is not system-authored"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT speaker_kind FROM local_goose_dialogue_completion_event WHERE sequence=4;")" = "human" ] || fail "sequence 4 is not human-authored"
    seq4_task=$(sqlite3 -readonly "$completion_sidecar" "SELECT task_id FROM local_goose_dialogue_completion_event WHERE sequence=4;")
    [ -n "$seq4_task" ] || fail "sequence 4 task is missing"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=3 AND head_task_id='$seq4_task';")" = "1" ] || fail "preferred head is not the current human sequence 4 task at revision 3"
}

verify_checkpoint
production_before=$(production_signature)
production_image_before=$(running_image)
provider_before=$(provider_total)
cycle_before=$(cycle_cursor)
task_shape_before=$(task_shape)

mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/run.XXXXXX")
baseline="$workspace/baseline"
mkdir -p -m 0700 "$baseline"

python3 - "$baseline" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

destination = Path(sys.argv[1])
for value in sys.argv[2:]:
    source = Path(value)
    target = destination / source.name
    src = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    try:
        dst = sqlite3.connect(target)
        try:
            src.backup(dst)
        finally:
            dst.close()
    finally:
        src.close()
    os.chmod(target, 0o600)
PY

[ "$(sqlite3 -readonly "$baseline/local-goose-dialogue-responder.db" 'PRAGMA user_version;')" = "1" ] || fail "baseline responder schema differs"
[ "$(sqlite3 -readonly "$baseline/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "2" ] || fail "baseline responder cursor differs"

printf '=== BUILD PROOF HELPER IN CONFINED PODMAN BUILDER ===\n'
proof_builder_image="localhost/gaudere-agent:observer-proof-builder-${agent_ref}"
"$podman_command" build \
    --target builder \
    --build-arg "BUILDER_IMAGE=registry.fedoraproject.org/fedora:44" \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --build-arg "GAUDERE_REF=$core_ref" \
    --tag "$proof_builder_image" \
    --file "$repository_root/Containerfile" \
    "$repository_root"

"$podman_command" run --rm --network none \
    --userns keep-id --user "$(id -u):$(id -g)" \
    --security-opt no-new-privileges --cap-drop all \
    -e PKG_CONFIG_PATH=/opt/gaudere/lib/pkgconfig \
    -e LD_LIBRARY_PATH=/opt/gaudere/lib \
    -v "$workspace:/proof:Z" \
    "$proof_builder_image" \
    sh -c '
        set -eu
        cflags=$(pkg-config --cflags gaudere-persistence-sqlite nlohmann_json)
        libs=$(pkg-config --libs gaudere-persistence-sqlite nlohmann_json)
        g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
            $cflags -I/src/gaudere-agent/src \
            /src/gaudere-agent/src/local_goose_dialogue_responder_observer_proof_main.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponderObserver.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponderStore.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponder.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueCompletionFeed.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueThreadStore.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueV2.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueV3.cpp \
            /src/gaudere-agent/src/Sha256.cpp \
            -o /proof/observer-proof \
            $libs -lsqlite3
        /proof/observer-proof --help 2>&1 | grep -Fq \
            "Commands: migrate, inspect, step, prepare-system, ack-prepared"
    '

clone_scenario()
{
    name=$1
    target="$workspace/$name"
    mkdir -p -m 0700 "$target"
    for source in "$baseline"/*.db; do
        cp --reflink=auto --sparse=always "$source" "$target/$(basename "$source")"
        chmod 600 "$target/$(basename "$source")"
    done
}

run_helper()
{
    directory=$1
    now=$2
    command=$3
    shift 3
    case "$directory" in
        "$workspace"/*) ;;
        *) fail "helper scenario is outside proof workspace" ;;
    esac
    relative=${directory#"$workspace/"}
    "$podman_command" run --rm --network none \
        --userns keep-id --user "$(id -u):$(id -g)" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --cap-drop all \
        -e LD_LIBRARY_PATH=/opt/gaudere/lib \
        -v "$workspace:/proof:Z" \
        "$proof_builder_image" \
        /proof/observer-proof "$command" \
        --thread-sidecar "/proof/$relative/local-goose-dialogue-thread.db" \
        --completion-sidecar "/proof/$relative/local-goose-dialogue-completion.db" \
        --responder-sidecar "/proof/$relative/local-goose-dialogue-responder.db" \
        --consumer-id "$consumer_id" \
        --thread-alias "$thread_alias" \
        --now-ms "$now" "$@"
}

verify_scenario_static()
{
    directory=$1
    [ "$(sqlite3 -readonly "$directory/state.db" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")" = "$provider_before" ] || fail "$directory provider total changed"
    [ "$(sqlite3 -readonly "$directory/local-goose-cycle.db" "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';")" = "$cycle_before" ] || fail "$directory cycle cursor changed"
    [ "$(sqlite3 -readonly "$directory/local-goose-cycle-stimulus.db" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] || fail "$directory stimulus ledger changed"
    [ "$(sqlite3 -readonly "$directory/state.db" "SELECT kind||'='||COUNT(*) FROM tasks GROUP BY kind ORDER BY kind;")" = "$task_shape_before" ] || fail "$directory task shape changed"
}

migrate_scenario()
{
    directory=$1
    run_helper "$directory" 2000000000000 migrate >/dev/null
    [ "$(sqlite3 -readonly "$directory/local-goose-dialogue-responder.db" 'PRAGMA user_version;')" = "2" ] || fail "$directory responder copy did not migrate to v2"
}

assert_human_held()
{
    directory=$1
    now=$2
    output=$(run_helper "$directory" "$now" step --expected-sequence 4) || {
        printf '%s\n' "$output" >&2
        fail "$directory human hold step failed"
    }
    printf '%s\n' "$output" | grep -q '^action=held$' || fail "$directory did not hold on human event"
    printf '%s\n' "$output" | grep -q '^classification=current_human$' || fail "$directory human event classification differs"
    printf '%s\n' "$output" | grep -q '^consumer_last_sequence=3$' || fail "$directory cursor advanced past human event"
    [ "$(sqlite3 -readonly "$directory/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] || fail "$directory durable cursor advanced past human event"
}

base_now=2000000000000

printf '=== OBSERVER NORMAL SYSTEM SKIP -> HUMAN HOLD ===\n'
clone_scenario normal
normal="$workspace/normal"
migrate_scenario "$normal"
normal_before=$(run_helper "$normal" "$base_now" inspect --expected-sequence 3) || fail "normal initial inspect failed"
printf '%s\n' "$normal_before" | grep -q '^classification=structural_system$' || fail "sequence 3 is not classified structural_system"
printf '%s\n' "$normal_before" | grep -q '^consumer_last_sequence=2$' || fail "normal initial cursor differs"
normal_step=$(run_helper "$normal" "$((base_now + 1))" step --expected-sequence 3) || fail "normal structural step failed"
printf '%s\n' "$normal_step"
printf '%s\n' "$normal_step" | grep -q '^action=acknowledged$' || fail "normal system event was not acknowledged"
printf '%s\n' "$normal_step" | grep -q '^classification=current_human$' || fail "normal step did not stop on human event"
printf '%s\n' "$normal_step" | grep -q '^next_sequence=4$' || fail "normal next sequence is not 4"
printf '%s\n' "$normal_step" | grep -q '^consumer_last_sequence=3$' || fail "normal cursor is not 3"
assert_human_held "$normal" "$((base_now + 2))"
assert_human_held "$normal" "$((base_now + 3))"
[ "$(sqlite3 -readonly "$normal/local-goose-dialogue-responder.db" 'SELECT kind||"|"||state FROM local_goose_dialogue_responder_disposition;')" = "0|1" ] || fail "normal disposition is not acknowledged structural system skip"
[ "$(sqlite3 -readonly "$normal/local-goose-dialogue-responder.db" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition;')" = "1" ] || fail "normal retry duplicated disposition"
verify_scenario_static "$normal"

printf '=== OBSERVER CRASH AFTER PREPARE / BEFORE ACK ===\n'
clone_scenario preack
preack="$workspace/preack"
migrate_scenario "$preack"
run_helper "$preack" "$((base_now + 10))" prepare-system --expected-sequence 3 >/dev/null || fail "pre-ACK disposition preparation failed"
[ "$(sqlite3 -readonly "$preack/local-goose-dialogue-responder.db" 'SELECT kind||"|"||state FROM local_goose_dialogue_responder_disposition;')" = "0|0" ] || fail "pre-ACK prepared disposition differs"
[ "$(sqlite3 -readonly "$preack/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "2" ] || fail "pre-ACK crash simulation changed cursor"
preack_recovery=$(run_helper "$preack" "$((base_now + 11))" step --expected-sequence 3) || fail "pre-ACK recovery failed"
printf '%s\n' "$preack_recovery" | grep -q '^action=acknowledged$' || fail "pre-ACK recovery did not ACK exact system event"
printf '%s\n' "$preack_recovery" | grep -q '^classification=current_human$' || fail "pre-ACK recovery did not stop on human event"
assert_human_held "$preack" "$((base_now + 12))"
verify_scenario_static "$preack"

printf '=== OBSERVER CRASH AFTER ACK / BEFORE DISPOSITION RECONCILIATION ===\n'
clone_scenario postack
postack="$workspace/postack"
migrate_scenario "$postack"
run_helper "$postack" "$((base_now + 20))" prepare-system --expected-sequence 3 >/dev/null || fail "post-ACK disposition preparation failed"
run_helper "$postack" "$((base_now + 21))" ack-prepared --expected-sequence 3 >/dev/null || fail "post-ACK crash simulation ACK failed"
[ "$(sqlite3 -readonly "$postack/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] || fail "post-ACK cursor is not 3"
[ "$(sqlite3 -readonly "$postack/local-goose-dialogue-responder.db" 'SELECT kind||"|"||state FROM local_goose_dialogue_responder_disposition;')" = "0|0" ] || fail "post-ACK disposition should still be prepared"
postack_recovery=$(run_helper "$postack" "$((base_now + 22))" step --expected-sequence 4) || fail "post-ACK reconciliation failed"
printf '%s\n' "$postack_recovery"
printf '%s\n' "$postack_recovery" | grep -q '^action=reconciled$' || fail "post-ACK recovery did not reconcile disposition"
printf '%s\n' "$postack_recovery" | grep -q '^classification=current_human$' || fail "post-ACK recovery did not remain on human event"
printf '%s\n' "$postack_recovery" | grep -q '^consumer_last_sequence=3$' || fail "post-ACK recovery advanced human event"
assert_human_held "$postack" "$((base_now + 23))"
[ "$(sqlite3 -readonly "$postack/local-goose-dialogue-responder.db" 'SELECT kind||"|"||state FROM local_goose_dialogue_responder_disposition;')" = "0|1" ] || fail "post-ACK disposition did not reconcile to acknowledged"
verify_scenario_static "$postack"

printf '=== VERIFY PRODUCTION PRESERVATION ===\n'
verify_checkpoint
production_after=$(production_signature)
[ "$production_after" = "$production_before" ] || {
    printf '%s\n' '--- production before ---' "$production_before" '--- production after ---' "$production_after" >&2
    fail "production logical signature changed during isolated proof"
}
[ "$(running_image)" = "$production_image_before" ] || fail "production image changed"
[ "$(provider_total)" = "$provider_before" ] || fail "production provider total changed"
[ "$(cycle_cursor)" = "$cycle_before" ] || fail "production cycle cursor changed"

printf 'agent_ref=%s\n' "$agent_ref"
printf 'core_ref=%s\n' "$core_ref"
printf 'production_image=%s\n' "$production_image_before"
printf 'observer_consumer_before=2\n'
printf 'observer_system_sequence=3\n'
printf 'observer_human_sequence=4\n'
printf 'observer_consumer_after_copy=3\n'
printf 'provider_total=%s\n' "$provider_before"
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_OBSERVER_ISOLATED=PASS\n'
proof_ok=1
