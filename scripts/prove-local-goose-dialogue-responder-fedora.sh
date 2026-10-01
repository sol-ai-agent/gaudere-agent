#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_production_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-c5a4d33177d1aaf915d82b98dc292fed91847bf88206cc87ffb00c00a39ff418}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
thread_alias=main
consumer_id=sol
root_task_id=cognition.local-goose-dialogue.v2:82164bd3699f19c5ff77046e6c60299d898ec225c5174b30c3d653aaa5510b19
v3_head_task_id=cognition.local-goose-dialogue.v3:3a28c66f9d089bccedbac2e51cd62e949905c6a7ec109423e3e9adefef52b65f
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}

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
production_responder_sidecar="$state_directory/local-goose-dialogue-responder.db"
proof_root="$data_home/gaudere/.dialogue-responder-isolated-proof"
workspace=""
proof_image=""
proof_ok=0

fail()
{
    printf 'gaudere dialogue responder isolated proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$proof_ok" = "1" ] && [ -n "$proof_image" ]; then
        "$podman_command" image rm "$proof_image" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'gaudere dialogue responder isolated proof: diagnostic directory preserved: %s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sed grep awk cut stat cp mkdir mktemp rm id basename chmod head sh tr; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done
[ ! -e "$production_responder_sidecar" ] || fail "production responder sidecar already exists before code deployment"

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
short_ref=$(printf '%s\n' "$agent_ref" | cut -c1-12)
host_uid=$(id -u)
host_gid=$(id -g)
proof_image="localhost/gaudere-agent:dialogue-responder-proof-$short_ref"

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

verify_production()
{
    [ "$(service_state)" = "active" ] || fail "production service is not active"
    [ "$(running_image)" = "$expected_production_image" ] || fail "production image differs from approved Stage 9I2/9I3 image"
    network=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
    [ "$network" = "none" ] || fail "production network is not none"
    [ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs"
    [ "$(cycle_cursor)" = "$expected_cycle_cursor" ] || fail "autonomous cycle cursor differs"
    [ "$(task_count cognition.local-goose-cycle.v1)" = "1" ] || fail "cycle Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v1)" = "1" ] || fail "V1 dialogue Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v2)" = "2" ] || fail "V2 dialogue Task count differs"
    [ "$(task_count cognition.local-goose-dialogue.v3)" = "1" ] || fail "V3 dialogue Task count differs"
    [ "$(stimulus_count)" = "0" ] || fail "stimulus ledger is not empty"
    [ "$(sqlite3 -readonly "$thread_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "thread sidecar schema differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'PRAGMA user_version;')" = "1" ] || fail "completion sidecar schema differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=1 AND root_task_id='$root_task_id' AND head_task_id='$v3_head_task_id';")" = "1" ] || fail "preferred production head differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_completion_event;')" = "2" ] || fail "completion event count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT next_revision FROM local_goose_dialogue_materialization WHERE thread_alias='$thread_alias';")" = "2" ] || fail "completion materialization differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_consumer_cursor;')" = "1" ] || fail "production completion consumer count differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "production sol consumer cursor differs"
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=2 AND thread_alias='$thread_alias' AND thread_revision=1 AND task_id='$v3_head_task_id';")" = "1" ] || fail "pending sequence 2 identity differs"
    [ ! -e "$production_responder_sidecar" ] || fail "production responder sidecar appeared"
}

verify_production
production_image_before=$(running_image)
production_network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
provider_before=$(provider_total)
cursor_before=$(cycle_cursor)

mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/run.XXXXXX")
baseline="$workspace/baseline"
mkdir -p -m 0700 "$baseline"

python3 - "$baseline" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

destination = Path(sys.argv[1])
sources = [Path(value) for value in sys.argv[2:]]

for source in sources:
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

[ "$(sqlite3 -readonly "$baseline/state.db" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")" = "$provider_before" ] || fail "baseline provider total differs"
[ "$(sqlite3 -readonly "$baseline/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "baseline consumer cursor differs"
[ "$(sqlite3 -readonly "$baseline/local-goose-dialogue-thread.db" "SELECT revision FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "1" ] || fail "baseline preferred revision differs"

printf '=== BUILD RESPONDER ISOLATED PROOF CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$proof_image" sh "$script_directory/build-image.sh"
"$podman_command" run --rm --network none --entrypoint /usr/bin/test "$proof_image" -x /usr/local/bin/gaudere-local-goose-dialogue-responder-proof || fail "candidate lacks responder proof helper"

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

run_proof_raw()
{
    directory=$1
    now=$2
    shift 2
    "$podman_command" run --rm         --network none         --userns=keep-id         --user "$host_uid:$host_gid"         --memory=1G         --pids-limit=32         --read-only         --security-opt=no-new-privileges         --cap-drop=all         --tmpfs /tmp:rw,nosuid,nodev,noexec,size=64m         --volume "$directory:/work:Z"         --entrypoint /usr/local/bin/gaudere-local-goose-dialogue-responder-proof         "$proof_image"         "$@"         --state /work/state.db         --thread-sidecar /work/local-goose-dialogue-thread.db         --completion-sidecar /work/local-goose-dialogue-completion.db         --responder-sidecar /work/local-goose-dialogue-responder.db         --model-sha256 "$model_sha256"         --consumer-id "$consumer_id"         --now-ms "$now"
}

run_proof()
{
    directory=$1
    now=$2
    shift 2
    output=$(run_proof_raw "$directory" "$now" "$@") || {
        printf '%s\n' "$output" >&2
        return 1
    }
    printf '%s\n' "$output"
}

run_proof_expect_conflict()
{
    directory=$1
    now=$2
    shift 2
    if output=$(run_proof_raw "$directory" "$now" "$@"); then
        printf '%s\n' "$output" >&2
        return 1
    else
        rc=$?
    fi
    [ "$rc" = "4" ] || {
        printf '%s\n' "$output" >&2
        return 1
    }
    printf '%s\n' "$output"
    printf '%s\n' "$output" | grep -q '^result=conflict$'
}

field()
{
    name=$1
    sed -n "s/^$name=//p" | head -n 1
}

scenario_static()
{
    directory=$1
    expected_v3=$2
    [ "$(sqlite3 -readonly "$directory/state.db" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")" = "$provider_before" ] || fail "$directory provider total changed"
    [ "$(sqlite3 -readonly "$directory/local-goose-cycle.db" "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';")" = "$cursor_before" ] || fail "$directory cycle cursor changed"
    [ "$(sqlite3 -readonly "$directory/local-goose-cycle-stimulus.db" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] || fail "$directory stimulus ledger changed"
    [ "$(sqlite3 -readonly "$directory/state.db" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v3';")" = "$expected_v3" ] || fail "$directory V3 Task count differs"
}

base_now=2000000000000

printf '=== SCENARIO NORMAL + IDEMPOTENT RETRY + EXHAUSTION ===\n'
clone_scenario normal
normal="$workspace/normal"
lease_normal=proof-normal
run_proof "$normal" "$base_now" lease-create     --lease-id "$lease_normal" --thread-alias "$thread_alias"     --speaker-id sol --message-kind feedback     --purpose "Isolated normal responder proof"     --max-turns 1 --lease-ttl-ms 600000 --min-interval-ms 0 >/dev/null
normal_prepare=$(run_proof "$normal" "$base_now" prepare     --lease-id "$lease_normal" --completion-sequence 2     --message-kind feedback     --message "Preuve isolée: réponse système bornée au head V3 de production copié."     --intent-ttl-ms 300000) || fail "normal prepare failed"
printf '%s\n' "$normal_prepare"
normal_intent=$(printf '%s\n' "$normal_prepare" | field intent_id)
[ -n "$normal_intent" ] || fail "normal intent id missing"
normal_dispatch=$(run_proof "$normal" "$((base_now + 10))" dispatch --intent-id "$normal_intent") || fail "normal dispatch failed"
printf '%s\n' "$normal_dispatch"
printf '%s\n' "$normal_dispatch" | grep -q '^result=accepted$' || fail "normal dispatch was not accepted"
printf '%s\n' "$normal_dispatch" | grep -q '^consumer_last_sequence=2$' || fail "normal dispatch did not ACK sequence 2"
printf '%s\n' "$normal_dispatch" | grep -q '^head_revision=2$' || fail "normal preferred head did not advance"
normal_task=$(printf '%s\n' "$normal_dispatch" | field task_id)
[ -n "$normal_task" ] || fail "normal responder Task id missing"
[ "$(sqlite3 -readonly "$normal/local-goose-dialogue-responder.db" "SELECT state||'|'||turns_committed FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_normal';")" = "1|1" ] || fail "normal lease is not exhausted after one turn"
[ "$(sqlite3 -readonly "$normal/local-goose-dialogue-responder.db" "SELECT state FROM local_goose_dialogue_responder_intent WHERE intent_id='$normal_intent';")" = "2" ] || fail "normal intent is not completed"
[ "$(sqlite3 -readonly "$normal/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "2" ] || fail "normal cursor did not persist sequence 2"
[ "$(sqlite3 -readonly "$normal/local-goose-dialogue-thread.db" "SELECT revision||'|'||head_task_id FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "2|$normal_task" ] || fail "normal preferred head differs"
scenario_static "$normal" 2
normal_retry=$(run_proof "$normal" "$((base_now + 20))" dispatch --intent-id "$normal_intent") || fail "normal duplicate retry failed"
printf '%s\n' "$normal_retry"
printf '%s\n' "$normal_retry" | grep -q '^result=duplicate$' || fail "normal retry is not duplicate"
scenario_static "$normal" 2

printf '=== SCENARIO CRASH AFTER TASK/CAS BEFORE ACCOUNTING/ACK ===\n'
clone_scenario recovery
recovery="$workspace/recovery"
lease_recovery=proof-recovery
run_proof "$recovery" "$base_now" lease-create     --lease-id "$lease_recovery" --thread-alias "$thread_alias"     --speaker-id sol --message-kind intervention     --purpose "Isolated responder crash recovery proof"     --max-turns 1 --lease-ttl-ms 600000 --min-interval-ms 0 >/dev/null
recovery_prepare=$(run_proof "$recovery" "$base_now" prepare     --lease-id "$lease_recovery" --completion-sequence 2     --message-kind intervention     --message "Preuve crash: successor durable avant accounting responder."     --intent-ttl-ms 300000) || fail "recovery prepare failed"
recovery_intent=$(printf '%s\n' "$recovery_prepare" | field intent_id)
[ -n "$recovery_intent" ] || fail "recovery intent id missing"
precrash=$(run_proof "$recovery" "$((base_now + 10))" preferred-from-intent --intent-id "$recovery_intent") || fail "pre-crash preferred submission failed"
printf '%s\n' "$precrash"
printf '%s\n' "$precrash" | grep -q '^result=accepted$' || fail "pre-crash preferred submission not accepted"
recovery_task=$(printf '%s\n' "$precrash" | field task_id)
[ -n "$recovery_task" ] || fail "recovery Task id missing"
[ "$(sqlite3 -readonly "$recovery/local-goose-dialogue-responder.db" "SELECT state FROM local_goose_dialogue_responder_intent WHERE intent_id='$recovery_intent';")" = "0" ] || fail "recovery intent did not remain prepared"
[ "$(sqlite3 -readonly "$recovery/local-goose-dialogue-responder.db" "SELECT turns_committed FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_recovery';")" = "0" ] || fail "recovery lease accounted turn before recovery"
[ "$(sqlite3 -readonly "$recovery/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "recovery trigger was ACKed before recovery"
[ "$(sqlite3 -readonly "$recovery/local-goose-dialogue-thread.db" "SELECT revision||'|'||head_task_id FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "2|$recovery_task" ] || fail "pre-crash preferred head differs"
scenario_static "$recovery" 2
recovered=$(run_proof "$recovery" "$((base_now + 20))" dispatch --intent-id "$recovery_intent") || fail "recovery dispatch failed"
printf '%s\n' "$recovered"
printf '%s\n' "$recovered" | grep -q '^result=accepted$' || fail "recovery was not accepted"
printf '%s\n' "$recovered" | grep -q '^consumer_last_sequence=2$' || fail "recovery did not ACK sequence 2"
[ "$(sqlite3 -readonly "$recovery/local-goose-dialogue-responder.db" "SELECT state FROM local_goose_dialogue_responder_intent WHERE intent_id='$recovery_intent';")" = "2" ] || fail "recovered intent is not completed"
[ "$(sqlite3 -readonly "$recovery/local-goose-dialogue-responder.db" "SELECT state||'|'||turns_committed FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_recovery';")" = "1|1" ] || fail "recovered lease accounting differs"
scenario_static "$recovery" 2

printf '=== SCENARIO STALE HEAD RACE ===\n'
clone_scenario stale
stale="$workspace/stale"
lease_stale=proof-stale
run_proof "$stale" "$base_now" lease-create     --lease-id "$lease_stale" --thread-alias "$thread_alias"     --speaker-id sol --message-kind feedback     --purpose "Isolated stale head responder proof"     --max-turns 1 --lease-ttl-ms 600000 --min-interval-ms 0 >/dev/null
stale_prepare=$(run_proof "$stale" "$base_now" prepare     --lease-id "$lease_stale" --completion-sequence 2     --message-kind feedback     --message "Ce responder doit perdre face au tour humain concurrent."     --intent-ttl-ms 300000) || fail "stale prepare failed"
stale_intent=$(printf '%s\n' "$stale_prepare" | field intent_id)
[ -n "$stale_intent" ] || fail "stale intent id missing"
compete=$(run_proof "$stale" "$((base_now + 10))" compete     --request-id proof-competing-human     --thread-alias "$thread_alias" --expected-revision 1     --speaker-kind human --speaker-id bertrand --message-kind dialogue     --message "Tour humain concurrent: le responder préparé doit devenir stale.") || fail "competing human preferred submission failed"
printf '%s\n' "$compete"
printf '%s\n' "$compete" | grep -q '^result=accepted$' || fail "competing human turn not accepted"
competing_task=$(printf '%s\n' "$compete" | field task_id)
[ -n "$competing_task" ] || fail "competing human Task id missing"
stale_result=$(run_proof_expect_conflict "$stale" "$((base_now + 20))" dispatch --intent-id "$stale_intent") || fail "stale responder dispatch did not fail closed"
printf '%s\n' "$stale_result"
[ "$(sqlite3 -readonly "$stale/local-goose-dialogue-responder.db" "SELECT state FROM local_goose_dialogue_responder_intent WHERE intent_id='$stale_intent';")" = "3" ] || fail "stale intent is not conflict terminal"
[ "$(sqlite3 -readonly "$stale/local-goose-dialogue-responder.db" "SELECT state||'|'||turns_committed FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_stale';")" = "0|0" ] || fail "stale lease consumed authority"
[ "$(sqlite3 -readonly "$stale/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "stale race ACKed trigger"
[ "$(sqlite3 -readonly "$stale/local-goose-dialogue-thread.db" "SELECT revision||'|'||head_task_id FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "2|$competing_task" ] || fail "stale race did not preserve competing head"
scenario_static "$stale" 2

printf '=== SCENARIO FINITE EXPIRY ===\n'
clone_scenario expiry
expiry="$workspace/expiry"
lease_expiry=proof-expiry
run_proof "$expiry" "$base_now" lease-create     --lease-id "$lease_expiry" --thread-alias "$thread_alias"     --speaker-id sol --message-kind feedback     --purpose "Isolated finite expiry responder proof"     --max-turns 1 --lease-ttl-ms 100 --min-interval-ms 0 >/dev/null
expiry_prepare=$(run_proof "$expiry" "$base_now" prepare     --lease-id "$lease_expiry" --completion-sequence 2     --message-kind feedback     --message "Ce responder doit expirer avant tout effet durable."     --intent-ttl-ms 100) || fail "expiry prepare failed"
expiry_intent=$(printf '%s\n' "$expiry_prepare" | field intent_id)
[ -n "$expiry_intent" ] || fail "expiry intent id missing"
expiry_result=$(run_proof_expect_conflict "$expiry" "$((base_now + 101))" dispatch --intent-id "$expiry_intent") || fail "expired responder dispatch did not fail closed"
printf '%s\n' "$expiry_result"
[ "$(sqlite3 -readonly "$expiry/local-goose-dialogue-responder.db" "SELECT state FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_expiry';")" = "2" ] || fail "expired lease did not terminalize"
[ "$(sqlite3 -readonly "$expiry/local-goose-dialogue-responder.db" "SELECT state FROM local_goose_dialogue_responder_intent WHERE intent_id='$expiry_intent';")" = "4" ] || fail "expired intent did not terminalize"
[ "$(sqlite3 -readonly "$expiry/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "1" ] || fail "expired responder ACKed trigger"
[ "$(sqlite3 -readonly "$expiry/local-goose-dialogue-thread.db" "SELECT revision||'|'||head_task_id FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias';")" = "1|$v3_head_task_id" ] || fail "expired responder changed preferred head"
scenario_static "$expiry" 1

verify_production
[ "$(running_image)" = "$production_image_before" ] || fail "production image changed during isolated proof"
[ "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" = "$production_network_before" ] || fail "production network changed during isolated proof"
[ "$(provider_total)" = "$provider_before" ] || fail "production provider total changed during isolated proof"
[ "$(cycle_cursor)" = "$cursor_before" ] || fail "production cycle cursor changed during isolated proof"

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$(running_image)"
printf 'NETWORK=none\n'
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'CYCLE_CURSOR=%s\n' "$(cycle_cursor)"
printf 'PRODUCTION_CONSUMER=%s:1\n' "$consumer_id"
printf 'PRODUCTION_NEXT_PENDING_SEQUENCE=2\n'
printf 'NORMAL_RESPONDER_TASK=%s\n' "$normal_task"
printf 'RECOVERED_RESPONDER_TASK=%s\n' "$recovery_task"
printf 'STALE_COMPETING_TASK=%s\n' "$competing_task"
printf 'FEDORA_LOCAL_GOOSE_DIALOGUE_RESPONDER_ISOLATED=PASS\n'

proof_ok=1
