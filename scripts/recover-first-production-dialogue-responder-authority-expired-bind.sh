#!/bin/sh
set -eu

authorization=${GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY_AUTHORIZATION:-}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
consumer_id=system-responder-v1
thread_alias=main
expected_event_id=cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844
expected_result_sha256=4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935
expected_task_id=cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa
expected_decision_id=cognition.local-goose-dialogue.responder-decision.v1:5e8bfd1668346f209eaec91f5888a3a9532a5bf5677674d4c2001f499e924153
expected_intent_id=cognition.local-goose-dialogue.responder-intent.v1:f82c3df882b60babea7d88a36589aad57d1f7b0b2b9ec4d208e8be8d01b71fae
expected_disposition_id=cognition.local-goose-dialogue.responder-disposition.v1:72741315708884592db29c29c4e6c9d750b6e49f3b15a1bd1b61f9c1e618236b

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
thread_sidecar="$state_directory/local-goose-dialogue-thread.db"
completion_sidecar="$state_directory/local-goose-dialogue-completion.db"
responder_sidecar="$state_directory/local-goose-dialogue-responder.db"
backup_script="$script_directory/backup-state.sh"
helper_containerfile="$repository_root/Containerfile.responder-authority-expired-bind-recovery"
main_containerfile="$repository_root/Containerfile"
observer_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-observer
authority_capability_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-authority-capability

workspace=""
backup=""
helper_image=""
builder_image=""
service_stopped=0
committed=0

fail()
{
    printf 'gaudere responder authority expired-bind recovery: FAIL: %s\n' "$*" >&2
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

network_mode()
{
    "$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null
}

provider_total()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';"
}

logical_dump_sha()
{
    sqlite3 -readonly "$1" '.dump' | sha256sum | awk '{print $1}'
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$rc" -ne 0 ]; then
        printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY=ABORTED\n' >&2
        [ -n "$backup" ] && printf 'BACKUP=%s\n' "$backup" >&2 || true
        [ -n "$workspace" ] && printf 'TRANSITION_WORKSPACE=%s\n' "$workspace" >&2 || true
        if [ "$service_stopped" = "1" ]; then
            if [ "$committed" = "0" ]; then
                "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
                printf 'SERVICE_RECOVERY=restart_attempted_without_production_commit\n' >&2
            else
                printf 'SERVICE_RECOVERY=left_stopped_after_production_commit_for_manual_review\n' >&2
            fi
        fi
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

verify_active_runtime()
{
    [ "$(service_state)" = "active" ] || fail "production service is not active"
    [ "$(running_image)" = "$expected_image" ] || fail "production image differs"
    [ "$(network_mode)" = "none" ] || fail "production network is not none"
}

verify_exact_sequence_four()
{
    [ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] \
        || fail "responder cursor is not exactly 3"
    sequence_row=$(sqlite3 -readonly "$completion_sidecar" "SELECT event_id||'|'||result_sha256||'|'||thread_alias||'|'||thread_revision||'|'||task_id||'|'||speaker_kind||'|'||speaker_id FROM local_goose_dialogue_completion_event WHERE sequence=4;")
    [ "$sequence_row" = "$expected_event_id|$expected_result_sha256|$thread_alias|3|$expected_task_id|human|bertrand" ] \
        || fail "completion sequence 4 exact evidence differs"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=3 AND head_task_id='$expected_task_id';")" = "1" ] \
        || fail "preferred head differs from exact sequence 4 evidence"
}

verify_observer_state()
{
    expected_disposition_state=$1
    output=$("$podman_command" exec gaudere-agent "$observer_binary" status \
        --thread-sidecar /var/lib/gaudere/local-goose-dialogue-thread.db \
        --completion-sidecar /var/lib/gaudere/local-goose-dialogue-completion.db \
        --responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db) \
        || fail "production observer status failed"
    printf '%s\n' "$output"
    printf '%s\n' "$output" | grep -q '^classification=intervention_owned$' \
        || fail "production sequence 4 is not intervention_owned"
    printf '%s\n' "$output" | grep -q '^observer_armed=no$' \
        || fail "production observer unexpectedly armed"
    printf '%s\n' "$output" | grep -q '^consumer_last_sequence=3$' \
        || fail "production responder cursor changed"
    printf '%s\n' "$output" | grep -q '^next_sequence=4$' \
        || fail "production sequence 4 is no longer pending"
    printf '%s\n' "$output" | grep -Fq "next_event_id=$expected_event_id" \
        || fail "production sequence 4 event differs"
    printf '%s\n' "$output" | grep -Fq "next_task_id=$expected_task_id" \
        || fail "production sequence 4 task differs"
    printf '%s\n' "$output" | grep -q '^next_speaker_kind=human$' \
        || fail "production sequence 4 speaker kind differs"
    printf '%s\n' "$output" | grep -q '^next_speaker_id=bertrand$' \
        || fail "production sequence 4 speaker id differs"
    printf '%s\n' "$output" | grep -q '^head_revision=3$' \
        || fail "production preferred-head revision changed"
    printf '%s\n' "$output" | grep -Fq "head_task_id=$expected_task_id" \
        || fail "production preferred-head Task changed"
    printf '%s\n' "$output" | grep -Fq "disposition_id=$expected_disposition_id" \
        || fail "production disposition identity differs"
    printf '%s\n' "$output" | grep -q '^disposition_kind=3$' \
        || fail "production disposition is not intervention kind"
    printf '%s\n' "$output" | grep -q "^disposition_state=$expected_disposition_state$" \
        || fail "production disposition state differs"
}

verify_dormant_runtime_surface()
{
    output=$("$podman_command" exec gaudere-agent "$authority_capability_binary" --check) \
        || fail "dormant authority capability probe failed"
    printf '%s\n' "$output"
    printf '%s\n' "$output" | grep -Fq 'AUTHORITY_DEFAULT=disabled' \
        || fail "production authority default changed"
    printf '%s\n' "$output" | grep -Fq 'RESPONDER_READABLE_SCHEMA=3' \
        || fail "production responder readable schema changed"
    printf '%s\n' "$output" | grep -Fq 'AUTHORITY_SCHEMA=3' \
        || fail "production authority schema capability changed"
    printf '%s\n' "$output" | grep -Fq 'STATE_ACCESS=absent' \
        || fail "production capability unexpectedly gained state access"
    printf '%s\n' "$output" | grep -Fq 'MUTATION_SURFACE=absent' \
        || fail "production capability unexpectedly gained mutation surface"
}

run_helper()
{
    directory=$1
    command=$2
    "$podman_command" run --rm --network none \
        --userns keep-id --user "$host_uid:$host_gid" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --security-opt label=disable \
        --cap-drop all \
        -v "$directory:/state:rw" \
        "$helper_image" "$command" \
        --state /state/state.db \
        --thread-sidecar /state/local-goose-dialogue-thread.db \
        --completion-sidecar /state/local-goose-dialogue-completion.db \
        --responder-sidecar /state/local-goose-dialogue-responder.db
}

require_pending_helper_output()
{
    output=$1
    printf '%s\n' "$output" | grep -q '^PHASE=expired_bound_pending_recovery$' \
        || fail "helper prestate phase differs"
    printf '%s\n' "$output" | grep -q '^RESPONDER_CURSOR=3$' \
        || fail "helper prestate cursor differs"
    printf '%s\n' "$output" | grep -q '^NEXT_SEQUENCE=4$' \
        || fail "helper prestate next sequence differs"
    printf '%s\n' "$output" | grep -q '^DECISION_STATE=intent_bound$' \
        || fail "helper prestate decision differs"
    printf '%s\n' "$output" | grep -q '^INTENT_STATE=prepared$' \
        || fail "helper prestate intent differs"
    printf '%s\n' "$output" | grep -q '^DISPOSITION_STATE=intervention_bound$' \
        || fail "helper prestate disposition differs"
    printf '%s\n' "$output" | grep -q '^LEASE_STATE=active$' \
        || fail "helper prestate lease durable state differs"
    printf '%s\n' "$output" | grep -q '^LEASE_TURNS_COMMITTED=0$' \
        || fail "helper prestate lease accounting differs"
    printf '%s\n' "$output" | grep -q '^SEQUENCE4_ACK=no$' \
        || fail "helper prestate unexpectedly ACKed sequence 4"
    printf '%s\n' "$output" | grep -q '^SUBMISSION_TASK=absent$' \
        || fail "helper prestate unexpectedly has a responder submission Task"
    printf '%s\n' "$output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY_INSPECT=PASS' \
        || fail "helper prestate inspect did not pass"
}

require_recovered_helper_output()
{
    output=$1
    printf '%s\n' "$output" | grep -q '^PHASE=expired_bound_recovered$' \
        || fail "helper recovered phase differs"
    printf '%s\n' "$output" | grep -q '^RESPONDER_CURSOR=3$' \
        || fail "helper recovered cursor differs"
    printf '%s\n' "$output" | grep -q '^NEXT_SEQUENCE=4$' \
        || fail "helper recovered next sequence differs"
    printf '%s\n' "$output" | grep -q '^DECISION_STATE=expired$' \
        || fail "helper recovered decision differs"
    printf '%s\n' "$output" | grep -q '^DECISION_TERMINAL_REASON=bound_intent_expired_before_dispatch$' \
        || fail "helper recovered decision reason differs"
    printf '%s\n' "$output" | grep -q '^INTENT_STATE=expired$' \
        || fail "helper recovered intent differs"
    printf '%s\n' "$output" | grep -q '^INTENT_TERMINAL_REASON=lease_or_intent_expired$' \
        || fail "helper recovered intent reason differs"
    printf '%s\n' "$output" | grep -q '^DISPOSITION_STATE=manual_review$' \
        || fail "helper recovered disposition differs"
    printf '%s\n' "$output" | grep -q '^DISPOSITION_TERMINAL_REASON=lease_or_intent_expired$' \
        || fail "helper recovered disposition reason differs"
    printf '%s\n' "$output" | grep -q '^LEASE_STATE=expired$' \
        || fail "helper recovered lease differs"
    printf '%s\n' "$output" | grep -q '^LEASE_TURNS_COMMITTED=0$' \
        || fail "helper recovered lease accounting differs"
    printf '%s\n' "$output" | grep -q '^SEQUENCE4_ACK=no$' \
        || fail "helper recovery ACKed sequence 4"
    printf '%s\n' "$output" | grep -q '^HEAD_REVISION=3$' \
        || fail "helper recovery advanced preferred head"
    printf '%s\n' "$output" | grep -q '^SUBMISSION_TASK=absent$' \
        || fail "helper recovery created a responder submission Task"
}

[ "$authorization" = "AUTHORIZED_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY" ] \
    || fail "explicit expired-bind recovery production authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 mktemp mkdir chmod sha256sum awk sed id sleep grep tr; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
for file in "$backup_script" "$helper_containerfile" "$main_containerfile" "$state_database" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done
[ "$("$podman_command" info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = "true" ] \
    || fail "expired-bind recovery gate requires rootless Podman"
[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] \
    || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] \
    || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] \
    || fail "gaudere-agent checkout must be clean"

git -C "$repository_root" fetch --quiet origin main
agent_ref=$(git -C "$repository_root" rev-parse HEAD)
remote_main=$(git -C "$repository_root" rev-parse origin/main)
[ "$agent_ref" = "$remote_main" ] || fail "local main is not exact current origin/main"
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
host_uid=$(id -u)
host_gid=$(id -g)
case "$agent_ref" in *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"
case "$core_ref" in *[!0-9a-f]*|'') fail "Core ref is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#core_ref}" -eq 40 ] || fail "Core ref is not 40 characters"
[ "$core_ref" = "1316cf68db93e4c91a7bd79fbd289b8f382f8659" ] \
    || fail "core pin differs from the approved authority lineage"

printf '=== VERIFY EXPIRED BOUND AUTHORITY PRESTATE ===\n'
verify_active_runtime
[ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs before recovery"
[ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "3" ] \
    || fail "production responder/authority sidecar is not schema v3"
verify_exact_sequence_four
verify_observer_state 2
verify_dormant_runtime_surface

printf '=== BUILD CONFINED EXPIRED-BIND RECOVERY HELPER ===\n'
builder_image="localhost/gaudere-agent:authority-expired-bind-recovery-builder-$agent_ref"
helper_image="localhost/gaudere-agent:authority-expired-bind-recovery-$agent_ref"
"$podman_command" build \
    --target builder \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --build-arg "GAUDERE_REF=$core_ref" \
    --tag "$builder_image" \
    --file "$main_containerfile" \
    "$repository_root"
"$podman_command" build \
    --build-arg "BUILDER_IMAGE=$builder_image" \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --build-arg "GAUDERE_REF=$core_ref" \
    --tag "$helper_image" \
    --file "$helper_containerfile" \
    "$repository_root"
[ "$("$podman_command" image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.agent.revision"}}')" = "$agent_ref" ] \
    || fail "expired-bind recovery helper agent provenance differs"
[ "$("$podman_command" image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.core.revision"}}')" = "$core_ref" ] \
    || fail "expired-bind recovery helper core provenance differs"
[ "$("$podman_command" image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.role"}}')" = "dialogue-responder-authority-expired-bind-recovery-v1" ] \
    || fail "expired-bind recovery helper role differs"
help_output=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs \
    --security-opt no-new-privileges --cap-drop all "$helper_image" --help) \
    || fail "expired-bind recovery helper help probe failed"
printf '%s\n' "$help_output"
printf '%s\n' "$help_output" | grep -Fq '<inspect|recover-expired-bind>' \
    || fail "expired-bind recovery helper surface differs"
! printf '%s\n' "$help_output" | grep -Eq 'prepare|grant|activate|bind-decision|dispatch-intent|ack' \
    || fail "expired-bind recovery helper exposes a forbidden authority phase"

printf '=== STOP PRODUCTION AND CAPTURE REAL BACKUP ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" != "active" ] || fail "production service did not stop"
backup=$(GAUDERE_STATE_DIR="$state_directory" "$backup_script") \
    || fail "production stopped-state backup failed"
[ -f "$backup" ] || fail "backup archive was not created"
printf 'BACKUP=%s\n' "$backup"

transition_root="$data_home/gaudere/.dialogue-responder-authority-expired-bind-recovery"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
rehearsal="$workspace/rehearsal"
mkdir -p -m 0700 "$rehearsal"

python3 - "$rehearsal" "$state_database" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" <<'PY'
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

state_sha_before=$(logical_dump_sha "$rehearsal/state.db")
thread_sha_before=$(logical_dump_sha "$rehearsal/local-goose-dialogue-thread.db")
completion_sha_before=$(logical_dump_sha "$rehearsal/local-goose-dialogue-completion.db")
responder_sha_before=$(logical_dump_sha "$rehearsal/local-goose-dialogue-responder.db")

printf '=== PROVE RECOVERY ON STOPPED-STATE REHEARSAL COPY ===\n'
pre_output=$(run_helper "$rehearsal" inspect) || fail "rehearsal pre-inspect failed"
printf '%s\n' "$pre_output"
require_pending_helper_output "$pre_output"

recovery_output=$(run_helper "$rehearsal" recover-expired-bind) \
    || fail "rehearsal expired-bind recovery failed"
printf '%s\n' "$recovery_output"
require_recovered_helper_output "$recovery_output"
printf '%s\n' "$recovery_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY=PASS' \
    || fail "rehearsal recovery PASS marker is missing"

post_output=$(run_helper "$rehearsal" inspect) || fail "rehearsal post-inspect failed"
printf '%s\n' "$post_output"
require_recovered_helper_output "$post_output"
printf '%s\n' "$post_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY_INSPECT=PASS' \
    || fail "rehearsal post-inspect PASS marker is missing"

[ "$(logical_dump_sha "$rehearsal/state.db")" = "$state_sha_before" ] \
    || fail "rehearsal recovery changed Task/provider state DB"
[ "$(logical_dump_sha "$rehearsal/local-goose-dialogue-thread.db")" = "$thread_sha_before" ] \
    || fail "rehearsal recovery changed dialogue thread DB"
[ "$(logical_dump_sha "$rehearsal/local-goose-dialogue-completion.db")" = "$completion_sha_before" ] \
    || fail "rehearsal recovery changed completion feed/ACK DB"
[ "$(logical_dump_sha "$rehearsal/local-goose-dialogue-responder.db")" != "$responder_sha_before" ] \
    || fail "rehearsal recovery did not change responder authority state"

printf '=== COMMIT ONLY RECOVERED RESPONDER/AUTHORITY SIDECAR ===\n'
python3 - "$rehearsal/local-goose-dialogue-responder.db" "$responder_sidecar" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
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
committed=1

live_stopped_output=$(run_helper "$state_directory" inspect) \
    || fail "stopped production post-commit inspect failed"
printf '%s\n' "$live_stopped_output"
require_recovered_helper_output "$live_stopped_output"
printf '%s\n' "$live_stopped_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY_INSPECT=PASS' \
    || fail "stopped production post-commit PASS marker is missing"
[ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total changed during recovery"
verify_exact_sequence_four

printf '=== RESTART UNCHANGED PRODUCTION RUNTIME AND PROVE DURABILITY ===\n'
"$systemctl_command" --user start "$service_name"
service_stopped=0
sleep 3
verify_active_runtime
verify_observer_state 4
verify_dormant_runtime_surface
[ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total changed after restart"
verify_exact_sequence_four

live_restart_output=$(run_helper "$state_directory" inspect) \
    || fail "restarted production recovery inspect failed"
printf '%s\n' "$live_restart_output"
require_recovered_helper_output "$live_restart_output"
printf '%s\n' "$live_restart_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY_INSPECT=PASS' \
    || fail "restarted production recovery PASS marker is missing"

trap - EXIT HUP INT TERM
printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$(running_image)"
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'DECISION_ID=%s\n' "$expected_decision_id"
printf 'INTENT_ID=%s\n' "$expected_intent_id"
printf 'DISPOSITION_ID=%s\n' "$expected_disposition_id"
printf 'BACKUP=%s\n' "$backup"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY=PASS\n'
