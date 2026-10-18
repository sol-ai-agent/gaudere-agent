#!/bin/sh
set -eu

authorization=${GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_CODE_DEPLOY_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_previous_image=${GAUDERE_EXPECTED_PREVIOUS_IMAGE:-ef14db77fa3b3d21ff3fed9b37ead611ca8bf533cff424da810ac6ac4d1a77eb}
consumer_id=system-responder-v1
thread_alias=main

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
capability_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-authority-capability

fail()
{
    printf 'gaudere responder authority code-only production deploy: FAIL: %s\n' "$*" >&2
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

verify_authority_absent_state()
{
    [ "$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] \
        || fail "stimulus ledger is not empty"
    responder_schema=$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')
    [ "$responder_schema" = "2" ] || fail "production responder schema is not exactly v2"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='local_goose_dialogue_responder_grant';")" = "0" ] \
        || fail "production unexpectedly contains responder grant table"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='local_goose_dialogue_responder_decision';")" = "0" ] \
        || fail "production unexpectedly contains responder decision table"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE state=0;')" = "0" ] \
        || fail "production unexpectedly has active responder lease"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE state=0;')" = "0" ] \
        || fail "production unexpectedly has prepared responder intent"

    responder_cursor=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")
    [ "$responder_cursor" = "3" ] || fail "production responder cursor is not exactly 3"
    sequence_four_kind=$(sqlite3 -readonly "$completion_sidecar" "SELECT speaker_kind FROM local_goose_dialogue_completion_event WHERE sequence=4;")
    [ "$sequence_four_kind" = "human" ] || fail "production completion sequence 4 is not human-authored"
    sequence_four_task=$(sqlite3 -readonly "$completion_sidecar" "SELECT task_id FROM local_goose_dialogue_completion_event WHERE sequence=4;")
    [ -n "$sequence_four_task" ] || fail "production completion sequence 4 task is missing"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=3 AND head_task_id='$sequence_four_task';")" = "1" ] \
        || fail "production sequence 4 is not the current preferred head"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND completion_sequence=3 AND kind=0 AND state=1;")" = "1" ] \
        || fail "production sequence 3 structural disposition differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND completion_sequence=4;")" = "0" ] \
        || fail "production sequence 4 already has responder disposition"
}

[ "$authorization" = "AUTHORIZED_DIALOGUE_RESPONDER_AUTHORITY_CODE_DEPLOY" ] \
    || fail "explicit authority code-deploy authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 install mkdir mktemp mv tar sed grep sleep stat tr sha256sum awk cmp; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
[ "$("$podman_command" info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = "true" ] \
    || fail "production deploy requires rootless Podman"
for file in "$backup_script" "$build_script" "$provenance_script" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" "$target_quadlet"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] \
    || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] \
    || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] \
    || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
case "$agent_ref" in *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"
case "$core_ref" in *[!0-9a-f]*|'') fail "gaudere.ref is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#core_ref}" -eq 40 ] || fail "gaudere.ref is not 40 characters"

printf '=== VERIFY AUTHORITY CODE-DEPLOY PRODUCTION PRESTATE ===\n'
[ "$(service_state)" = "active" ] || fail "production service must be active before deploy"
previous_image=$(running_image)
[ "$previous_image" = "$expected_previous_image" ] || fail "production image differs from expected pre-authority image"
[ "$(network_mode)" = "none" ] || fail "production network is not none"
provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total differs from expected value"
verify_authority_absent_state

state_sha_before=$(logical_dump_sha "$state_database")
cycle_sha_before=$(logical_dump_sha "$cycle_sidecar")
stimulus_sha_before=$(logical_dump_sha "$stimulus_sidecar")
thread_sha_before=$(logical_dump_sha "$thread_sidecar")
completion_sha_before=$(logical_dump_sha "$completion_sidecar")
responder_sha_before=$(logical_dump_sha "$responder_sidecar")

[ "$(grep -c '^Image=' "$target_quadlet")" = "1" ] || fail "installed Quadlet must contain exactly one Image= line"
grep -Fq 'Network=none' "$target_quadlet" || fail "installed Quadlet lacks Network=none"
! grep -Fq 'gaudere-local-goose-dialogue-responder-authority-capability' "$target_quadlet" \
    || fail "authority capability probe is unexpectedly wired into production profile"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$target_quadlet" \
    || fail "production profile unexpectedly contains provider authority"

transition_root="$data_home/gaudere/.dialogue-responder-authority-code-deploy"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
previous_quadlet="$workspace/gaudere-agent.container.before"
candidate_quadlet="$workspace/gaudere-agent.container.candidate"
install -m 0600 "$target_quadlet" "$previous_quadlet"

candidate_tag="localhost/gaudere-agent:dialogue-responder-authority-code-$agent_ref"
printf '=== BUILD DORMANT AUTHORITY CAPABILITY CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$candidate_tag" sh "$build_script"
provenance_output=$(PODMAN="$podman_command" sh "$provenance_script" "$candidate_tag" "$agent_ref" "$core_ref") \
    || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
candidate_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p' | tail -n 1)
case "$candidate_id" in sha256:*) ;; *) fail "provenance verifier did not return immutable sha256 image ID" ;; esac
candidate_normalized=$(printf '%s\n' "$candidate_id" | sed 's/^sha256://')

printf '=== VERIFY CANDIDATE AUTHORITY CAPABILITY IS DORMANT ===\n'
capability_output=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs \
    --security-opt=no-new-privileges --cap-drop=all \
    --entrypoint "$capability_binary" "$candidate_id" --check) \
    || fail "candidate authority capability probe failed"
printf '%s\n' "$capability_output"
printf '%s\n' "$capability_output" | grep -Fq 'AUTHORITY_DEFAULT=disabled' || fail "candidate authority default is not disabled"
printf '%s\n' "$capability_output" | grep -Fq 'RESPONDER_OWNED_SCHEMA=2' || fail "candidate responder owned schema differs"
printf '%s\n' "$capability_output" | grep -Fq 'RESPONDER_READABLE_SCHEMA=3' || fail "candidate responder readable schema differs"
printf '%s\n' "$capability_output" | grep -Fq 'AUTHORITY_SCHEMA=3' || fail "candidate authority schema differs"
printf '%s\n' "$capability_output" | grep -Fq 'STATE_ACCESS=absent' || fail "candidate capability unexpectedly exposes state access"
printf '%s\n' "$capability_output" | grep -Fq 'MUTATION_SURFACE=absent' || fail "candidate capability unexpectedly exposes mutation surface"
printf '%s\n' "$capability_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_CAPABILITY=PASS' || fail "candidate capability PASS marker missing"

agent_help=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs \
    --security-opt=no-new-privileges --cap-drop=all \
    --entrypoint /usr/local/bin/gaudere-agent "$candidate_id" --help) \
    || fail "candidate gaudere-agent --help failed"
! printf '%s\n' "$agent_help" | grep -Eq -- 'decision-authority|responder-authority|authority-grant|authority-decision' \
    || fail "candidate agent unexpectedly exposes authority activation arguments"

control_status=0
control_help=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs \
    --security-opt=no-new-privileges --cap-drop=all \
    --entrypoint /usr/local/bin/gaudere-control "$candidate_id" --help 2>&1) || control_status=$?
[ "$control_status" = "0" ] || [ "$control_status" = "2" ] \
    || fail "candidate control help returned unexpected status $control_status"
! printf '%s\n' "$control_help" | grep -Eq -- 'dialogue-responder-(authority|grant|decision)' \
    || fail "candidate control surface unexpectedly exposes decision authority"

python3 - "$target_quadlet" "$candidate_quadlet" "$candidate_id" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1])
destination = Path(sys.argv[2])
candidate = sys.argv[3]
old = source.read_text(encoding="utf-8").splitlines(keepends=True)
indices = [i for i, line in enumerate(old) if line.startswith("Image=")]
if len(indices) != 1:
    raise SystemExit("installed Quadlet must contain exactly one Image=")
new = list(old)
i = indices[0]
ending = "\n" if new[i].endswith("\n") else ""
new[i] = f"Image={candidate}{ending}"
destination.write_text("".join(new), encoding="utf-8")
def normalize(lines):
    return ["Image=<allowed>\n" if line.startswith("Image=") and line.endswith("\n") else "Image=<allowed>" if line.startswith("Image=") else line for line in lines]
if normalize(old) != normalize(new):
    raise SystemExit("authority code deploy attempted a mutation beyond Image=")
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
    if [ "$committed" = "1" ]; then exit "$status"; fi
    if [ "$service_stopped" = "0" ] && [ "$state_mutated" = "0" ] && [ "$profile_mutated" = "0" ]; then exit "$status"; fi
    printf 'gaudere responder authority code deploy: recovery starting\n' >&2
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
state_mutated=1
install -m 0600 "$candidate_quadlet" "$target_quadlet"
profile_mutated=1
cmp -s "$candidate_quadlet" "$target_quadlet" || fail "installed Quadlet differs from approved image-only candidate"
"$systemctl_command" --user daemon-reload

printf '=== START PRODUCTION WITH AUTHORITY CODE PRESENT BUT UNWIRED ===\n'
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "candidate production service did not become active"
[ "$(running_image)" = "$candidate_normalized" ] || fail "running image is not approved candidate"
sleep 3
[ "$(service_state)" = "active" ] || fail "candidate production service did not remain active"
[ "$(network_mode)" = "none" ] || fail "production network changed from none"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during code-only deploy"
verify_authority_absent_state

[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "code-only deploy changed main state database"
[ "$(logical_dump_sha "$cycle_sidecar")" = "$cycle_sha_before" ] || fail "code-only deploy changed cycle sidecar"
[ "$(logical_dump_sha "$stimulus_sidecar")" = "$stimulus_sha_before" ] || fail "code-only deploy changed stimulus sidecar"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "code-only deploy changed dialogue thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "code-only deploy changed completion state"
[ "$(logical_dump_sha "$responder_sidecar")" = "$responder_sha_before" ] || fail "code-only deploy changed responder schema/state"
cmp -s "$candidate_quadlet" "$target_quadlet" || fail "production Quadlet changed after candidate activation"
! grep -Fq 'gaudere-local-goose-dialogue-responder-authority-capability' "$target_quadlet" \
    || fail "authority capability probe became wired into production profile"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$target_quadlet" \
    || fail "provider authority appeared after deploy"

printf '=== VERIFY DEPLOYED CAPABILITY REMAINS PASSIVE ===\n'
production_capability=$("$podman_command" exec gaudere-agent "$capability_binary" --check) \
    || fail "deployed authority capability probe failed"
printf '%s\n' "$production_capability"
printf '%s\n' "$production_capability" | grep -Fq 'AUTHORITY_DEFAULT=disabled' || fail "deployed authority default is not disabled"
printf '%s\n' "$production_capability" | grep -Fq 'STATE_ACCESS=absent' || fail "deployed capability unexpectedly exposes state access"
printf '%s\n' "$production_capability" | grep -Fq 'MUTATION_SURFACE=absent' || fail "deployed capability unexpectedly exposes mutation surface"
printf '%s\n' "$production_capability" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_CAPABILITY=PASS' || fail "deployed capability PASS marker missing"
verify_authority_absent_state
[ "$(logical_dump_sha "$responder_sidecar")" = "$responder_sha_before" ] || fail "capability probe changed responder state"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'CANDIDATE_IMAGE=%s\n' "$candidate_normalized"
printf 'ROLLBACK_IMAGE=%s\n' "$previous_image"
printf 'NETWORK=%s\n' "$(network_mode)"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'PRODUCTION_RESPONDER_SCHEMA=%s\n' "$responder_schema"
printf 'PRODUCTION_RESPONDER_CURSOR=%s\n' "$responder_cursor"
printf 'PRODUCTION_NEXT_PENDING_SEQUENCE=4\n'
printf 'PRODUCTION_NEXT_CLASSIFICATION=current_human\n'
printf 'PRODUCTION_AUTHORITY_TABLES=absent\n'
printf 'PRODUCTION_ACTIVE_LEASES=0\n'
printf 'PRODUCTION_PREPARED_INTENTS=0\n'
printf 'AUTHORITY_CAPABILITY_PRESENT=yes\n'
printf 'AUTHORITY_MODE=disabled-unwired\n'
printf 'AUTHORITY_STATE_ACCESS=absent\n'
printf 'AUTHORITY_MUTATION_SURFACE=absent\n'
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_CODE_PRODUCTION=PASS\n'
