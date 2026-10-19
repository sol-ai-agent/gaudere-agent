#!/bin/sh
set -eu

authorization=${GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_AUTHORIZATION:-}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
consumer_id=system-responder-v1
thread_alias=main
expected_event_id=cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844
expected_result_sha256=4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935
expected_task_id=cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa
expected_grant_request_id=first-production-responder-authority-grant-v1
expected_grant_speaker_id=sol
expected_grant_turns=1

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
helper_containerfile="$repository_root/Containerfile.responder-authority-first-grant"
main_containerfile="$repository_root/Containerfile"
observer_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-observer
authority_capability_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-authority-capability

fail()
{
    printf 'gaudere responder authority first grant activation: FAIL: %s\n' "$*" >&2
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

table_dump_sha()
{
    sqlite3 -readonly "$1" ".dump $2" | sha256sum | awk '{print $1}'
}

active_lease_count()
{
    sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE state=0;'
}

prepared_intent_count()
{
    sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE state=0;'
}

grant_count()
{
    sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_grant;'
}

decision_count()
{
    sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;'
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
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND event_id='$expected_event_id';")" = "0" ] \
        || fail "sequence 4 already has responder disposition"
}

verify_common_state()
{
    [ "$(network_mode)" = "none" ] || fail "production network is not none"
    [ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs"
    [ "$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] \
        || fail "stimulus ledger is not empty"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "3" ] \
        || fail "responder schema is not exactly v3"
    verify_exact_sequence_four
    [ "$(decision_count)" = "0" ] || fail "authority decisions already exist"
    [ "$(prepared_intent_count)" = "0" ] || fail "prepared responder intents already exist"
}

verify_prestate()
{
    verify_common_state
    [ "$(grant_count)" = "0" ] || fail "authority grants already exist"
    [ "$(active_lease_count)" = "0" ] || fail "active responder lease already exists"
}

verify_prepared_state()
{
    verify_common_state
    [ "$(grant_count)" = "1" ] || fail "prepared authority grant count is not exactly one"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_grant WHERE request_id='$expected_grant_request_id' AND state=0 AND lease_id IS NULL;")" = "1" ] \
        || fail "first authority grant is not durably prepared"
    [ "$(active_lease_count)" = "0" ] || fail "prepared authority grant activated a lease implicitly"
}

verify_activated_state()
{
    verify_common_state
    [ "$(grant_count)" = "1" ] || fail "activated authority grant count is not exactly one"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_grant WHERE request_id='$expected_grant_request_id' AND state=1 AND lease_id IS NOT NULL;")" = "1" ] \
        || fail "first authority grant is not durably activated"
    [ "$(active_lease_count)" = "1" ] || fail "first authority grant does not have exactly one active lease"
    lease_id=$(sqlite3 -readonly "$responder_sidecar" "SELECT lease_id FROM local_goose_dialogue_responder_grant WHERE request_id='$expected_grant_request_id';")
    [ -n "$lease_id" ] || fail "activated authority grant has no lease id"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_id' AND state=0 AND thread_alias='$thread_alias' AND turns_committed=0 AND json_extract(document,'$.speaker_kind')='system' AND json_extract(document,'$.speaker_id')='$expected_grant_speaker_id' AND json_extract(document,'$.max_system_turns')=$expected_grant_turns;")" = "1" ] \
        || fail "activated authority lease envelope differs"
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

verify_observer_holds_sequence_four()
{
    output=$("$podman_command" exec gaudere-agent "$observer_binary" status \
        --thread-sidecar /var/lib/gaudere/local-goose-dialogue-thread.db \
        --completion-sidecar /var/lib/gaudere/local-goose-dialogue-completion.db \
        --responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db) \
        || fail "production observer status failed"
    printf '%s\n' "$output"
    printf '%s\n' "$output" | grep -q '^classification=current_human$' \
        || fail "production next event is not current_human"
    printf '%s\n' "$output" | grep -q '^observer_armed=no$' \
        || fail "production observer unexpectedly armed"
    printf '%s\n' "$output" | grep -q '^consumer_last_sequence=3$' \
        || fail "production responder cursor changed"
    printf '%s\n' "$output" | grep -q '^next_sequence=4$' \
        || fail "production sequence 4 is no longer pending"
}

verify_dormant_runtime_surface()
{
    output=$("$podman_command" exec gaudere-agent "$authority_capability_binary" --check) \
        || fail "dormant authority capability probe failed"
    printf '%s\n' "$output"
    printf '%s\n' "$output" | grep -Fq 'AUTHORITY_DEFAULT=disabled' \
        || fail "production authority default changed"
    printf '%s\n' "$output" | grep -Fq 'STATE_ACCESS=absent' \
        || fail "production capability unexpectedly gained state access"
    printf '%s\n' "$output" | grep -Fq 'MUTATION_SURFACE=absent' \
        || fail "production capability unexpectedly gained mutation surface"
}

[ "$authorization" = "AUTHORIZED_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT" ] \
    || fail "explicit first authority grant production authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 mktemp mkdir mv tar sha256sum awk sed stat id sleep grep tr; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
for file in "$backup_script" "$helper_containerfile" "$main_containerfile" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done
[ "$("$podman_command" info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = "true" ] \
    || fail "first authority grant gate requires rootless Podman"
[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] \
    || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] \
    || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] \
    || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
host_uid=$(id -u)
host_gid=$(id -g)
case "$agent_ref" in *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"
case "$core_ref" in *[!0-9a-f]*|'') fail "Core ref is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#core_ref}" -eq 40 ] || fail "Core ref is not 40 characters"

printf '=== VERIFY FIRST AUTHORITY GRANT PRESTATE ===\n'
[ "$(service_state)" = "active" ] || fail "production service must be active before grant gate"
[ "$(running_image)" = "$expected_image" ] || fail "production image differs from expected dormant-authority image"
verify_prestate
verify_observer_holds_sequence_four
verify_dormant_runtime_surface

state_sha_before=$(logical_dump_sha "$state_database")
cycle_sha_before=$(logical_dump_sha "$cycle_sidecar")
stimulus_sha_before=$(logical_dump_sha "$stimulus_sidecar")
thread_sha_before=$(logical_dump_sha "$thread_sidecar")
completion_sha_before=$(logical_dump_sha "$completion_sidecar")
intent_sha_before=$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_intent)
disposition_sha_before=$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_disposition)
lease_count_before=$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;')

printf '=== BUILD CONFINED FIRST AUTHORITY GRANT HELPER ===\n'
builder_image="localhost/gaudere-agent:authority-first-grant-builder-$agent_ref"
helper_image="localhost/gaudere-agent:authority-first-grant-$agent_ref"
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
    || fail "first authority grant helper agent provenance differs"
[ "$("$podman_command" image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.core.revision"}}')" = "$core_ref" ] \
    || fail "first authority grant helper core provenance differs"
help_output=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs \
    --security-opt no-new-privileges --cap-drop all "$helper_image" --help) \
    || fail "first authority grant helper help probe failed"
printf '%s\n' "$help_output"
printf '%s\n' "$help_output" | grep -Fq '<inspect|prepare-grant|activate-grant>' \
    || fail "first authority grant helper surface differs"
! printf '%s\n' "$help_output" | grep -Eq 'decision|bind|dispatch|ack' \
    || fail "first authority grant helper exposes later authority phases"

transition_root="$data_home/gaudere/.dialogue-responder-authority-first-grant"
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

printf '=== REHEARSE PREPARE / REOPEN / ACTIVATE ON COPIED STATE ===\n'
rehearsal_pre=$(run_helper "$rehearsal" inspect) || fail "copy rehearsal pre-inspect failed"
printf '%s\n' "$rehearsal_pre"
printf '%s\n' "$rehearsal_pre" | grep -Fq 'GRANT_STATE=absent' || fail "copy rehearsal grant prestate differs"
rehearsal_prepare=$(run_helper "$rehearsal" prepare-grant) || fail "copy rehearsal grant preparation failed"
printf '%s\n' "$rehearsal_prepare"
printf '%s\n' "$rehearsal_prepare" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_PREPARE=PASS' || fail "copy rehearsal prepare marker differs"
rehearsal_reopen=$(run_helper "$rehearsal" inspect) || fail "copy rehearsal prepared-state reopen failed"
printf '%s\n' "$rehearsal_reopen"
printf '%s\n' "$rehearsal_reopen" | grep -Fq 'GRANT_STATE=prepared' || fail "prepared grant did not survive copy reopen"
printf '%s\n' "$rehearsal_reopen" | grep -Fq 'LEASE_ID=none' || fail "copy reopen implicitly activated a lease"
rehearsal_activate=$(run_helper "$rehearsal" activate-grant) || fail "copy rehearsal grant activation failed"
printf '%s\n' "$rehearsal_activate"
printf '%s\n' "$rehearsal_activate" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_ACTIVATE=PASS' || fail "copy rehearsal activate marker differs"
rehearsal_final=$(run_helper "$rehearsal" inspect) || fail "copy rehearsal activated-state reopen failed"
printf '%s\n' "$rehearsal_final"
printf '%s\n' "$rehearsal_final" | grep -Fq 'GRANT_STATE=activated' || fail "activated grant did not survive copy reopen"
printf '%s\n' "$rehearsal_final" | grep -Fq 'LEASE_MAX_SYSTEM_TURNS=1' || fail "copy rehearsal lease turn bound differs"
printf '%s\n' "$rehearsal_final" | grep -Fq 'LEASE_TURNS_COMMITTED=0' || fail "copy rehearsal consumed a lease turn"
[ "$(sqlite3 -readonly "$rehearsal/local-goose-dialogue-responder.db" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;')" = "0" ] || fail "copy rehearsal created an authority decision"
[ "$(sqlite3 -readonly "$rehearsal/local-goose-dialogue-responder.db" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE state=0;')" = "0" ] || fail "copy rehearsal created a prepared intent"
[ "$(sqlite3 -readonly "$rehearsal/local-goose-dialogue-responder.db" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND event_id='$expected_event_id';")" = "0" ] || fail "copy rehearsal created sequence 4 disposition"
[ "$(sqlite3 -readonly "$rehearsal/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] || fail "copy rehearsal ACKed sequence 4"

verify_prestate
[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "rehearsal changed production state database"
[ "$(logical_dump_sha "$cycle_sidecar")" = "$cycle_sha_before" ] || fail "rehearsal changed production cycle state"
[ "$(logical_dump_sha "$stimulus_sidecar")" = "$stimulus_sha_before" ] || fail "rehearsal changed production stimulus state"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "rehearsal changed production thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "rehearsal changed production completion state"

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
        if [ "$service_stopped" = "1" ]; then
            "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
        fi
        exit "$status"
    fi
    printf 'gaudere responder authority first grant activation: recovery starting\n' >&2
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
    printf 'recovery_provider_total=%s\n' "$(provider_total 2>/dev/null || true)" >&2
    printf 'recovery_workspace=%s\n' "$workspace" >&2
    exit "$status"
}
trap recover EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '=== STOP / BACKUP BEFORE FIRST AUTHORITY GRANT PREPARATION ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly"
verify_prestate
backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ] || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"

printf '=== PREPARE FIRST DURABLE AUTHORITY GRANT ONLY ===\n'
state_mutated=1
prepare_output=$(run_helper "$state_directory" prepare-grant) || fail "production authority grant preparation failed"
printf '%s\n' "$prepare_output"
printf '%s\n' "$prepare_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_PREPARE=PASS' || fail "production prepare marker differs"
verify_prepared_state
[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "grant preparation changed state database"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "grant preparation changed thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "grant preparation changed completion state"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_intent)" = "$intent_sha_before" ] || fail "grant preparation changed responder intents"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_disposition)" = "$disposition_sha_before" ] || fail "grant preparation changed responder dispositions"

printf '=== RESTART PRODUCTION TO PROVE PREPARED GRANT DOES NOT SELF-ACTIVATE ===\n'
"$systemctl_command" --user start "$service_name"
service_stopped=0
[ "$(service_state)" = "active" ] || fail "production service did not become active after grant preparation"
sleep 3
[ "$(service_state)" = "active" ] || fail "production service did not remain active after grant preparation"
[ "$(running_image)" = "$expected_image" ] || fail "production image changed after grant preparation"
verify_prepared_state
verify_observer_holds_sequence_four
verify_dormant_runtime_surface
prepared_reopen=$(run_helper "$state_directory" inspect) || fail "prepared production grant one-shot reopen failed"
printf '%s\n' "$prepared_reopen"
printf '%s\n' "$prepared_reopen" | grep -Fq 'GRANT_STATE=prepared' || fail "prepared production grant did not survive reopen"
printf '%s\n' "$prepared_reopen" | grep -Fq 'LEASE_ID=none' || fail "service restart implicitly activated first authority grant"

printf '=== STOP / EXPLICITLY ACTIVATE FIRST AUTHORITY GRANT ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop before explicit grant activation"
verify_prepared_state
activate_output=$(run_helper "$state_directory" activate-grant) || fail "production authority grant activation failed"
printf '%s\n' "$activate_output"
printf '%s\n' "$activate_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_ACTIVATE=PASS' || fail "production activate marker differs"
verify_activated_state
[ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;')" = "$((lease_count_before + 1))" ] || fail "explicit grant activation did not add exactly one lease"
[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "grant activation changed state database"
[ "$(logical_dump_sha "$cycle_sidecar")" = "$cycle_sha_before" ] || fail "grant activation changed cycle state"
[ "$(logical_dump_sha "$stimulus_sidecar")" = "$stimulus_sha_before" ] || fail "grant activation changed stimulus state"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "grant activation changed thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "grant activation changed completion state"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_intent)" = "$intent_sha_before" ] || fail "grant activation changed responder intents"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_disposition)" = "$disposition_sha_before" ] || fail "grant activation changed responder dispositions"

printf '=== RESTART SAME DORMANT-AUTHORITY PRODUCTION IMAGE ===\n'
"$systemctl_command" --user start "$service_name"
service_stopped=0
[ "$(service_state)" = "active" ] || fail "production service did not become active after grant activation"
sleep 3
[ "$(service_state)" = "active" ] || fail "production service did not remain active after grant activation"
[ "$(running_image)" = "$expected_image" ] || fail "production image changed during first authority grant gate"
verify_activated_state
verify_observer_holds_sequence_four
verify_dormant_runtime_surface
final_inspect=$(run_helper "$state_directory" inspect) || fail "activated production grant one-shot reopen failed"
printf '%s\n' "$final_inspect"
printf '%s\n' "$final_inspect" | grep -Fq 'GRANT_STATE=activated' || fail "activated production grant did not survive final reopen"
printf '%s\n' "$final_inspect" | grep -Fq 'LEASE_MAX_SYSTEM_TURNS=1' || fail "final active lease turn bound differs"
printf '%s\n' "$final_inspect" | grep -Fq 'LEASE_TURNS_COMMITTED=0' || fail "final active lease already consumed a turn"

committed=1
trap - EXIT HUP INT TERM

lease_id=$(sqlite3 -readonly "$responder_sidecar" "SELECT lease_id FROM local_goose_dialogue_responder_grant WHERE request_id='$expected_grant_request_id';")
grant_id=$(sqlite3 -readonly "$responder_sidecar" "SELECT grant_id FROM local_goose_dialogue_responder_grant WHERE request_id='$expected_grant_request_id';")
lease_expires_at_ms=$(sqlite3 -readonly "$responder_sidecar" "SELECT json_extract(document,'$.expires_at_ms') FROM local_goose_dialogue_responder_lease WHERE lease_id='$lease_id';")
printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$expected_image"
printf 'NETWORK=none\n'
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'PRODUCTION_RESPONDER_SCHEMA=3\n'
printf 'PRODUCTION_RESPONDER_CURSOR=3\n'
printf 'PRODUCTION_NEXT_PENDING_SEQUENCE=4\n'
printf 'PRODUCTION_NEXT_CLASSIFICATION=current_human\n'
printf 'PRODUCTION_AUTHORITY_GRANTS=1\n'
printf 'PRODUCTION_AUTHORITY_GRANT_STATE=activated\n'
printf 'PRODUCTION_AUTHORITY_GRANT_ID=%s\n' "$grant_id"
printf 'PRODUCTION_ACTIVE_LEASES=1\n'
printf 'PRODUCTION_AUTHORITY_LEASE_ID=%s\n' "$lease_id"
printf 'PRODUCTION_AUTHORITY_LEASE_MAX_SYSTEM_TURNS=1\n'
printf 'PRODUCTION_AUTHORITY_LEASE_TURNS_COMMITTED=0\n'
printf 'PRODUCTION_AUTHORITY_LEASE_EXPIRES_AT_MS=%s\n' "$lease_expires_at_ms"
printf 'PRODUCTION_AUTHORITY_DECISIONS=0\n'
printf 'PRODUCTION_PREPARED_INTENTS=0\n'
printf 'SEQUENCE4_DISPOSITION=absent\n'
printf 'SEQUENCE4_ACK=no\n'
printf 'AUTHORITY_RUNTIME_MODE=disabled-unwired\n'
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_ACTIVATION=PASS\n'
