#!/bin/sh
set -eu

authorization=${GAUDERE_LOCAL_GOOSE_DIALOGUE_V2_DEPLOY_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_previous_image=${GAUDERE_EXPECTED_PREVIOUS_IMAGE:-c83667ebd552032dcc92c62516bbfe7b22d91ac87432d63a7ffd4c0ba287a897}
expected_cycle_cursor=${GAUDERE_EXPECTED_CYCLE_CURSOR:-3|2|0||||cognition.local-goose-cycle.v1:cba9c7a6fd40d71b4a30c81a14b6fb366aefde2360099f38c736929456292f7f|}
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
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
backup_script="$script_directory/backup-state.sh"
build_script="$script_directory/build-image.sh"
provenance_script="$script_directory/verify-image-provenance.sh"

fail()
{
    printf 'gaudere Local Goose dialogue v2 production deploy: FAIL: %s\n' "$*" >&2
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

v1_dialogue_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v1';"
}

v2_dialogue_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-dialogue.v2';"
}

cycle_task_count()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';"
}

stimulus_count()
{
    sqlite3 -readonly "$stimulus_sidecar" "SELECT COUNT(*) FROM local_goose_cycle_stimuli;"
}

[ "$authorization" = "AUTHORIZED_LOCAL_GOOSE_DIALOGUE_V2_CODE_DEPLOY" ] || fail "explicit V2 code-deploy authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 install mkdir mktemp rm sed grep sleep stat id tail; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

for file in "$backup_script" "$build_script" "$provenance_script" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$target_quadlet"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
host_uid=$(id -u)

[ "$(service_state)" = "active" ] || fail "production service must be active before deploy"
previous_image=$(running_image)
[ "$previous_image" = "$expected_previous_image" ] || fail "production image differs from expected pre-deploy image"

network_before=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production network is not none"

provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ] || fail "provider total is $provider_before, expected $expected_provider_total"
cursor_before=$(cycle_cursor)
[ "$cursor_before" = "$expected_cycle_cursor" ] || fail "production Local Goose cycle cursor differs from expected dormant cursor"
v1_dialogue_before=$(v1_dialogue_task_count)
[ "$v1_dialogue_before" = "1" ] || fail "production V1 dialogue Task count is not exactly 1"
v2_dialogue_before=$(v2_dialogue_task_count)
[ "$v2_dialogue_before" = "0" ] || fail "production already contains Local Goose dialogue V2 Tasks"
cycle_tasks_before=$(cycle_task_count)
stimuli_before=$(stimulus_count)
[ "$stimuli_before" = "0" ] || fail "production stimulus ledger is not empty"

grep -q '^Network=none$' "$target_quadlet" || fail "installed Quadlet lacks Network=none"
grep -q -- '--local-goose-model ' "$target_quadlet" || fail "installed Quadlet lacks Local Goose model"
grep -q -- '--local-goose-model-sha256 ' "$target_quadlet" || fail "installed Quadlet lacks Local Goose model SHA256"
grep -q -- '--control-socket /tmp/gaudere-control.sock' "$target_quadlet" || fail "installed Quadlet lacks control socket"
grep -q -- '--local-goose-cycle-stimulus-sidecar /var/lib/gaudere/local-goose-cycle-stimulus.db' "$target_quadlet" || fail "installed Quadlet lacks idle stimulus sidecar"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$target_quadlet" || fail "production profile unexpectedly contains provider authority"

transition_root="$data_home/gaudere/.local-goose-dialogue-deploy"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
previous_quadlet="$workspace/gaudere-agent.container.before"
candidate_quadlet="$workspace/gaudere-agent.container.candidate"
install -m 0600 "$target_quadlet" "$previous_quadlet"

candidate_tag="localhost/gaudere-agent:local-goose-dialogue-v2-$agent_ref"
printf '=== BUILD DIALOGUE CODE-DEPLOY CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$candidate_tag" sh "$build_script"

provenance_output=$(PODMAN="$podman_command" sh "$provenance_script" "$candidate_tag" "$agent_ref" "$core_ref") || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
candidate_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p' | tail -n 1)
case "$candidate_id" in
    sha256:*) ;;
    *) fail "provenance verifier did not return immutable sha256 image ID" ;;
esac
candidate_normalized=$(printf '%s\n' "$candidate_id" | sed 's/^sha256://')

"$podman_command" run --rm --network none --read-only --read-only-tmpfs --security-opt=no-new-privileges --cap-drop=all --entrypoint /usr/bin/test "$candidate_id" -x /usr/local/bin/gaudere-control || fail "candidate lacks control client"
control_help=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs --security-opt=no-new-privileges --cap-drop=all --entrypoint /usr/local/bin/gaudere-control "$candidate_id" --help 2>&1)
printf '%s\n' "$control_help" | grep -q 'local-message' || fail "candidate control client lost local-message"
printf '%s\n' "$control_help" | grep -q 'local-thread-start' || fail "candidate control client lacks local-thread-start"
printf '%s\n' "$control_help" | grep -q 'local-thread-next' || fail "candidate control client lacks local-thread-next"

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
    raise SystemExit("dialogue deploy attempted a mutation beyond Image=")
PY

backup_archive=""
service_stopped=0
profile_mutated=0
committed=0

recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = "1" ]; then
        exit "$status"
    fi
    if [ "$service_stopped" = "0" ] && [ "$profile_mutated" = "0" ]; then
        exit "$status"
    fi

    printf 'gaudere Local Goose dialogue v2 deploy: recovery starting\n' >&2
    "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
    if [ "$profile_mutated" = "1" ]; then
        install -m 0600 "$previous_quadlet" "$target_quadlet" || true
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
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

install -m 0600 "$candidate_quadlet" "$target_quadlet"
profile_mutated=1
"$systemctl_command" --user daemon-reload

printf '=== START PRODUCTION WITH DIALOGUE V2 CODE WIRED BUT IDLE ===\n'
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "candidate production service did not become active"
[ "$(running_image)" = "$candidate_normalized" ] || fail "running image is not approved candidate"
sleep 2
[ "$(service_state)" = "active" ] || fail "candidate production service did not remain active"

network_after=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "none" ] || fail "production network changed from none"
provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ] || fail "provider total changed during dialogue code deploy"
cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ] || fail "autonomous cycle cursor changed during dialogue code deploy"
v1_dialogue_after=$(v1_dialogue_task_count)
[ "$v1_dialogue_after" = "$v1_dialogue_before" ] || fail "code-only deploy changed V1 dialogue Task count"
v2_dialogue_after=$(v2_dialogue_task_count)
[ "$v2_dialogue_after" = "$v2_dialogue_before" ] || fail "code-only deploy created a V2 dialogue Task"
cycle_tasks_after=$(cycle_task_count)
[ "$cycle_tasks_after" = "$cycle_tasks_before" ] || fail "code-only deploy changed Local Goose cycle Task count"
stimuli_after=$(stimulus_count)
[ "$stimuli_after" = "$stimuli_before" ] || fail "code-only deploy changed stimulus ledger"

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
printf 'LOCAL_GOOSE_DIALOGUE_V1_TASKS=%s\n' "$v1_dialogue_after"
printf 'LOCAL_GOOSE_DIALOGUE_V2_TASKS=%s\n' "$v2_dialogue_after"
printf 'STIMULI=%s\n' "$stimuli_after"
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_LOCAL_GOOSE_DIALOGUE_V2_CODE_DEPLOY=PASS\n'
