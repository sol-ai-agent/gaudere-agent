#!/bin/sh
set -eu

authorization=${GAUDERE_DIALOGUE_RESPONDER_OBSERVER_FIRST_ACTIVATION_AUTHORIZATION:-}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-ef14db77fa3b3d21ff3fed9b37ead611ca8bf533cff424da810ac6ac4d1a77eb}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
expected_responder_v1_sha=${GAUDERE_EXPECTED_RESPONDER_V1_SHA256:-77ed92357b6c7535cacf19af134bbba083e962bbd6324fadc6a5e77bf4e8d943}
root_task_id=cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19
system_task_id=cognition.local-goose-dialogue.v3:b5ff22b1696e7fd7a84005fd292a3b69d4d5ffd936cdb6507af1cb220e201813
human_task_id=cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa
human_event_id=cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844
human_result_sha256=4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935
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
backup_script="$script_directory/backup-state.sh"
migrator_containerfile="$repository_root/Containerfile.responder-schema-migrator"
observer_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-observer
migrator_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-schema-migrator

fail()
{
    printf 'gaudere dialogue responder observer first production activation: FAIL: %s\n' "$*" >&2
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

logical_dump_sha()
{
    sqlite3 -readonly "$1" '.dump' | sha256sum | awk '{print $1}'
}

legacy_responder_sha()
{
    sqlite3 -readonly "$1" "SELECT 'lease|'||quote(lease_id)||'|'||state||'|'||turns_committed||'|'||quote(document) FROM local_goose_dialogue_responder_lease ORDER BY lease_id; SELECT 'intent|'||quote(intent_id)||'|'||state||'|'||quote(document) FROM local_goose_dialogue_responder_intent ORDER BY intent_id;" \
        | sha256sum | awk '{print $1}'
}

verify_static_state()
{
    [ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs"
    [ "$(cycle_cursor)" = "$expected_cycle_cursor" ] || fail "autonomous cycle cursor differs"
    [ "$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] || fail "stimulus ledger is not empty"
    [ "$(task_count cognition.local-goose-cycle.v1)" = "1" ] || fail "cycle Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v1)" = "1" ] || fail "dialogue V1 Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v2)" = "2" ] || fail "dialogue V2 Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v3)" = "3" ] || fail "dialogue V3 Task count differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "thread schema differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "completion schema differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=3 AND root_task_id='$root_task_id' AND head_task_id='$human_task_id';")" = "1" ] || fail "preferred human head differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')" = "4" ] || fail "completion event count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")" = "4" ] || fail "completion materialization differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')" = "2" ] || fail "completion consumer count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$manual_consumer_id';")" = "1" ] || fail "sol cursor differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=3 AND thread_alias='$thread_alias' AND thread_revision=2 AND task_id='$system_task_id' AND speaker_kind='system';")" = "1" ] || fail "sequence 3 is not the canonical system completion"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=4 AND event_id='$human_event_id' AND result_sha256='$human_result_sha256' AND thread_alias='$thread_alias' AND thread_revision=3 AND task_id='$human_task_id' AND speaker_kind='human' AND speaker_id='bertrand';")" = "1" ] || fail "sequence 4 is not the canonical current human completion"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;')" = "1" ] || fail "historical responder lease count differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE state=1 AND turns_committed=1;')" = "1" ] || fail "historical responder lease state differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent;')" = "1" ] || fail "historical responder intent count differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE state=2;')" = "1" ] || fail "historical responder intent state differs"
}

verify_responder_v1()
{
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "responder sidecar is not schema v1"
    tables=$(sqlite3 -readonly "$responder_sidecar" "SELECT group_concat(name, '|') FROM (SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'local_goose_dialogue_responder_%' ORDER BY name);")
    [ "$tables" = "local_goose_dialogue_responder_intent|local_goose_dialogue_responder_lease" ] || fail "responder v1 table layout differs"
    [ "$(logical_dump_sha "$responder_sidecar")" = "$expected_responder_v1_sha" ] || fail "responder v1 logical state differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "2" ] || fail "responder cursor is not 2"
}

verify_responder_v2_after_ack()
{
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "responder sidecar is not schema v2"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "3" ] || fail "responder cursor is not exactly 3"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition;')" = "1" ] || fail "expected exactly one responder disposition"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$responder_consumer_id' AND completion_sequence=3 AND kind=0 AND state=1;")" = "1" ] || fail "sequence 3 disposition is not acknowledged structural system skip"
}

run_migrator()
{
    directory=$1
    "$podman_command" run --rm --network none \
        --userns keep-id --user "$host_uid:$host_gid" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --security-opt label=disable \
        --cap-drop all \
        -v "$directory:/state:rw" \
        "$migrator_image" migrate \
        --responder-sidecar /state/local-goose-dialogue-responder.db \
        --authorize-v1-to-v2
}

run_observer()
{
    directory=$1
    command=$2
    shift 2
    "$podman_command" run --rm --network none \
        --userns keep-id --user "$host_uid:$host_gid" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --security-opt label=disable \
        --cap-drop all \
        -v "$directory:/state:rw" \
        --entrypoint "$observer_binary" \
        "sha256:$expected_image" "$command" \
        --thread-sidecar /state/local-goose-dialogue-thread.db \
        --completion-sidecar /state/local-goose-dialogue-completion.db \
        --responder-sidecar /state/local-goose-dialogue-responder.db "$@"
}

[ "$authorization" = "AUTHORIZED_FIRST_PRODUCTION_DIALOGUE_RESPONDER_OBSERVER" ] || fail "explicit first observer activation authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 mktemp mkdir mv tar sha256sum awk sed stat id; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
for file in "$backup_script" "$migrator_containerfile" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done
[ "$("$podman_command" info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = "true" ] || fail "rootless Podman is required"
[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
host_uid=$(id -u)
host_gid=$(id -g)

printf '=== VERIFY FIRST OBSERVER ACTIVATION PRESTATE ===\n'
[ "$(service_state)" = "active" ] || fail "production service must be active"
[ "$(running_image)" = "$expected_image" ] || fail "production image differs from observer code-deploy image"
[ "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" = "none" ] || fail "production network is not none"
verify_static_state
verify_responder_v1
legacy_before=$(legacy_responder_sha "$responder_sidecar")
thread_before=$(logical_dump_sha "$thread_sidecar")
completion_before=$(logical_dump_sha "$completion_sidecar")
seq3_event_id=$(sqlite3 -readonly "$completion_sidecar" 'SELECT event_id FROM local_goose_dialogue_completion_event WHERE sequence=3;')
seq3_result_sha256=$(sqlite3 -readonly "$completion_sidecar" 'SELECT result_sha256 FROM local_goose_dialogue_completion_event WHERE sequence=3;')
[ -n "$seq3_event_id" ] && [ "${#seq3_result_sha256}" = "64" ] || fail "sequence 3 exact evidence is incomplete"

printf '=== BUILD CONFINED MIGRATION-ONLY HELPER ===\n'
migrator_image="localhost/gaudere-agent:responder-schema-migrator-$agent_ref"
"$podman_command" build \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --tag "$migrator_image" \
    --file "$migrator_containerfile" \
    "$repository_root"
[ "$("$podman_command" image inspect "$migrator_image" --format '{{index .Config.Labels "io.gaudere.agent.revision"}}')" = "$agent_ref" ] || fail "migrator image provenance label differs"
help_output=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs --security-opt no-new-privileges --cap-drop all "$migrator_image" --help) || fail "migrator help probe failed"
printf '%s\n' "$help_output"
printf '%s\n' "$help_output" | grep -Fq 'migrate --responder-sidecar PATH --authorize-v1-to-v2' || fail "migrator surface differs"

proof_root="$data_home/gaudere/.dialogue-responder-observer-first-activation"
mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/transition.XXXXXX")
rehearsal="$workspace/rehearsal"
mkdir -p -m 0700 "$rehearsal"

python3 - "$rehearsal" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

destination = Path(sys.argv[1])
for raw in sys.argv[2:]:
    source = Path(raw)
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

printf '=== REHEARSE EXACT MIGRATION AND SEQUENCE 3 RETIREMENT ON COPY ===\n'
run_migrator "$rehearsal" >/dev/null
[ "$(sqlite3 -readonly "$rehearsal/local-goose-dialogue-responder.db" 'PRAGMA user_version;')" = "2" ] || fail "rehearsal migration did not reach schema v2"
rehearsal_status=$(run_observer "$rehearsal" status) || fail "rehearsal observer status failed"
printf '%s\n' "$rehearsal_status" | grep -q '^classification=structural_system$' || fail "rehearsal sequence 3 is not structural_system"
printf '%s\n' "$rehearsal_status" | grep -q '^consumer_last_sequence=2$' || fail "rehearsal initial responder cursor differs"
rehearsal_step=$(run_observer "$rehearsal" step \
    --expected-sequence 3 \
    --expected-event-id "$seq3_event_id" \
    --expected-result-sha256 "$seq3_result_sha256" \
    --authorize-structural-retirement) || fail "rehearsal observer step failed"
printf '%s\n' "$rehearsal_step"
printf '%s\n' "$rehearsal_step" | grep -q '^action=acknowledged$' || fail "rehearsal did not ACK exact sequence 3"
printf '%s\n' "$rehearsal_step" | grep -q '^classification=current_human$' || fail "rehearsal did not stop on current human event"
printf '%s\n' "$rehearsal_step" | grep -q '^consumer_last_sequence=3$' || fail "rehearsal responder cursor is not 3"
printf '%s\n' "$rehearsal_step" | grep -q '^next_sequence=4$' || fail "rehearsal next event is not sequence 4"
[ "$(legacy_responder_sha "$rehearsal/local-goose-dialogue-responder.db")" = "$legacy_before" ] || fail "rehearsal migration changed historical lease/intent rows"
[ "$(sqlite3 -readonly "$rehearsal/local-goose-dialogue-responder.db" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$responder_consumer_id' AND completion_sequence=3 AND kind=0 AND state=1;")" = "1" ] || fail "rehearsal disposition differs"

verify_static_state
verify_responder_v1
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_before" ] || fail "rehearsal changed production thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_before" ] || fail "rehearsal changed production completion state"

backup_archive=""
state_mutated=0
service_stopped=0
committed=0
recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = "1" ]; then exit "$status"; fi
    if [ "$state_mutated" = "0" ]; then
        if [ "$service_stopped" = "1" ]; then "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true; fi
        exit "$status"
    fi
    printf 'gaudere dialogue responder observer activation: recovery starting\n' >&2
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

printf '=== STOP AND BACK UP PRODUCTION STATE ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly"
verify_static_state
verify_responder_v1
backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ] || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"

printf '=== MIGRATE RESPONDER SIDECAR V1 -> V2 UNDER EXPLICIT AUTHORITY ===\n'
state_mutated=1
migration_output=$(run_migrator "$state_directory") || fail "production responder schema migration failed"
printf '%s\n' "$migration_output"
[ "$(legacy_responder_sha "$responder_sidecar")" = "$legacy_before" ] || fail "schema migration changed historical lease/intent rows"
[ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "production responder schema did not reach v2"
[ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition;')" = "0" ] || fail "schema migration fabricated historical dispositions"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$responder_consumer_id';")" = "2" ] || fail "schema migration moved responder cursor"

printf '=== RETIRE EXACT SYSTEM COMPLETION SEQUENCE 3 ===\n'
activation_output=$(run_observer "$state_directory" step \
    --expected-sequence 3 \
    --expected-event-id "$seq3_event_id" \
    --expected-result-sha256 "$seq3_result_sha256" \
    --authorize-structural-retirement) || fail "production observer sequence 3 step failed"
printf '%s\n' "$activation_output"
printf '%s\n' "$activation_output" | grep -q '^action=acknowledged$' || fail "production observer did not ACK exact sequence 3"
printf '%s\n' "$activation_output" | grep -q '^classification=current_human$' || fail "production observer did not stop on current human sequence 4"
printf '%s\n' "$activation_output" | grep -q '^consumer_last_sequence=3$' || fail "production responder cursor is not 3 after step"
printf '%s\n' "$activation_output" | grep -q '^next_sequence=4$' || fail "production observer next event is not sequence 4"
verify_responder_v2_after_ack
[ "$(legacy_responder_sha "$responder_sidecar")" = "$legacy_before" ] || fail "observer activation changed historical lease/intent rows"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_before" ] || fail "observer activation changed preferred thread state"

printf '=== RESTART PRODUCTION AND VERIFY HUMAN HOLD ===\n'
"$systemctl_command" --user start "$service_name"
service_stopped=0
[ "$(service_state)" = "active" ] || fail "production service did not become active"
sleep 3
[ "$(service_state)" = "active" ] || fail "production service did not remain active"
[ "$(running_image)" = "$expected_image" ] || fail "production image changed during observer activation"
[ "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" = "none" ] || fail "production network changed from none"
verify_static_state
verify_responder_v2_after_ack
[ "$(legacy_responder_sha "$responder_sidecar")" = "$legacy_before" ] || fail "service restart changed historical responder rows"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_before" ] || fail "service restart changed thread state"
post_status=$("$podman_command" exec gaudere-agent "$observer_binary" status \
    --thread-sidecar /var/lib/gaudere/local-goose-dialogue-thread.db \
    --completion-sidecar /var/lib/gaudere/local-goose-dialogue-completion.db \
    --responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db) || fail "post-activation observer status failed"
printf '%s\n' "$post_status"
printf '%s\n' "$post_status" | grep -q '^classification=current_human$' || fail "post-activation next event is not current_human"
printf '%s\n' "$post_status" | grep -q '^observer_armed=no$' || fail "observer is unexpectedly armed after one-shot activation"
printf '%s\n' "$post_status" | grep -q '^consumer_last_sequence=3$' || fail "post-activation cursor differs"
printf '%s\n' "$post_status" | grep -q '^next_sequence=4$' || fail "post-activation sequence 4 is not pending"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$expected_image"
printf 'NETWORK=none\n'
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'CYCLE_CURSOR=%s\n' "$(cycle_cursor)"
printf 'DIALOGUE_THREAD_REVISION=3\n'
printf 'DIALOGUE_THREAD_HEAD=%s\n' "$human_task_id"
printf 'DIALOGUE_COMPLETION_EVENTS=4\n'
printf 'DIALOGUE_COMPLETION_MATERIALIZATION=4\n'
printf 'DIALOGUE_CONSUMER_SOL=%s:1\n' "$manual_consumer_id"
printf 'DIALOGUE_CONSUMER_RESPONDER=%s:3\n' "$responder_consumer_id"
printf 'DIALOGUE_RESPONDER_SCHEMA=2\n'
printf 'DIALOGUE_RESPONDER_DISPOSITIONS=1\n'
printf 'DIALOGUE_RESPONDER_SEQUENCE_3_DISPOSITION=structural_skip_system:acknowledged\n'
printf 'DIALOGUE_RESPONDER_NEXT_PENDING_SEQUENCE=4\n'
printf 'DIALOGUE_RESPONDER_NEXT_CLASSIFICATION=current_human\n'
printf 'DIALOGUE_RESPONDER_OBSERVER_ARMED=no\n'
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_OBSERVER_FIRST_PRODUCTION_ACTIVATION=PASS\n'
