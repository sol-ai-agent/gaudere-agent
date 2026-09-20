#!/bin/sh
set -eu

authorization=${GAUDERE_LOCAL_GOOSE_STIMULUS_DEPLOY_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_previous_image=${GAUDERE_EXPECTED_PREVIOUS_IMAGE:-fac2cc351d173411ef1f798d6ab99ed03b14e16674e417e0f907429c93560c97}
expected_cycle_revision=${GAUDERE_EXPECTED_CYCLE_REVISION:-3}
expected_cycle_generation=${GAUDERE_EXPECTED_CYCLE_GENERATION:-2}
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
    printf 'gaudere Local Goose stimulus production deploy: FAIL: %s\n' "$*" >&2
    exit 1
}

normalize_image()
{
    printf '%s\n' "$1" | sed 's/^sha256://'
}

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

running_image()
{
    "$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null         | sed 's/^sha256://'
}

provider_total()
{
    sqlite3 -readonly "$state_database"         "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';"
}

cycle_cursor()
{
    sqlite3 -readonly "$cycle_sidecar"         "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';"
}

cycle_task_count()
{
    sqlite3 -readonly "$state_database"         "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';"
}

[ "$authorization" = "AUTHORIZED_LOCAL_GOOSE_STIMULUS_CODE_DEPLOY" ]     || fail "explicit code-deploy authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3         sha256sum tar install mkdir mktemp mv rm sed grep awk sleep date tail         stat id; do
    command -v "$command" >/dev/null 2>&1         || fail "required command not found: $command"
done

[ -f "$backup_script" ] && [ ! -L "$backup_script" ]     || fail "backup script is missing or unsafe"
[ -f "$build_script" ] && [ ! -L "$build_script" ]     || fail "build script is missing or unsafe"
[ -f "$provenance_script" ] && [ ! -L "$provenance_script" ]     || fail "provenance verifier is missing or unsafe"
[ -f "$state_database" ] && [ ! -L "$state_database" ]     || fail "production state database is missing or unsafe"
[ -f "$cycle_sidecar" ] && [ ! -L "$cycle_sidecar" ]     || fail "production Local Goose cycle sidecar is missing or unsafe"
[ ! -e "$stimulus_sidecar" ]     || fail "production stimulus sidecar already exists; refusing deploy replay"
[ -f "$target_quadlet" ] && [ ! -L "$target_quadlet" ]     || fail "installed Quadlet is missing or unsafe"

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ]     || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ]     || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ]     || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
host_uid=$(id -u)
host_gid=$(id -g)

[ "$(service_state)" = "active" ]     || fail "production service must be active before deploy"
previous_running_image=$(running_image)
[ "$previous_running_image" = "$expected_previous_image" ]     || fail "production image differs from expected pre-deploy image"

network_before=$("$podman_command" inspect gaudere-agent     --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ]     || fail "production network is not none before deploy"

provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ]     || fail "provider total before deploy is $provider_before, expected $expected_provider_total"

cursor_before=$(cycle_cursor)
[ -n "$cursor_before" ] || fail "production cycle cursor is missing"
cursor_revision=$(printf '%s\n' "$cursor_before" | awk -F'|' '{print $1}')
cursor_generation=$(printf '%s\n' "$cursor_before" | awk -F'|' '{print $2}')
cursor_state=$(printf '%s\n' "$cursor_before" | awk -F'|' '{print $3}')
[ "$cursor_revision" = "$expected_cycle_revision" ]     || fail "cycle revision is $cursor_revision, expected $expected_cycle_revision"
[ "$cursor_generation" = "$expected_cycle_generation" ]     || fail "cycle generation is $cursor_generation, expected $expected_cycle_generation"
[ "$cursor_state" = "0" ] || fail "production cycle is not dormant"

tasks_before=$(cycle_task_count)

transition_root="$data_home/gaudere/.local-goose-stimulus-deploy"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
previous_quadlet="$workspace/gaudere-agent.container.before"
rendered_quadlet="$workspace/gaudere-agent.container.candidate"
install -m 0600 "$target_quadlet" "$previous_quadlet"

candidate_tag="localhost/gaudere-agent:local-goose-stimulus-$agent_ref"
printf '=== BUILD STIMULUS CODE-DEPLOY CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$candidate_tag" sh "$build_script"

provenance_output=$(PODMAN="$podman_command" sh "$provenance_script"     "$candidate_tag" "$agent_ref" "$core_ref")     || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
candidate_id=$(printf '%s\n' "$provenance_output"     | sed -n 's/^image_id=//p' | tail -n 1)
case "$candidate_id" in
    sha256:*) ;;
    *) fail "provenance verifier did not return immutable sha256 image ID" ;;
esac
candidate_id_normalized=$(normalize_image "$candidate_id")

"$podman_command" run --rm --network none --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --entrypoint /usr/local/bin/gaudere-agent "$candidate_id" --help     | grep -q -- '--local-goose-cycle-stimulus-sidecar PATH'     || fail "candidate main service lacks bounded stimulus capability"

"$podman_command" run --rm --network none --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --entrypoint /usr/local/bin/gaudere-local-goose-cycle-stimulus-init     "$candidate_id" --help 2>&1     | grep -q -- '--stimulus-sidecar PATH'     || fail "candidate lacks bounded stimulus initializer"

"$podman_command" run --rm --network none --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --entrypoint /usr/local/bin/gaudere-control "$candidate_id"     --socket /tmp/unused stimulate-local-goose-cycle 2>&1     | grep -q 'Usage:'     || fail "candidate gaudere-control lacks bounded stimulus command"

python3 - "$target_quadlet" "$rendered_quadlet" "$candidate_id" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
candidate_id = sys.argv[3]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
plain = [line.rstrip("\n") for line in lines]
images = [i for i, line in enumerate(plain) if line.startswith("Image=")]
execs = [i for i, line in enumerate(plain) if line.startswith("Exec=")]
if len(images) != 1 or len(execs) != 1:
    raise SystemExit("installed Quadlet must contain exactly one Image= and Exec=")
exec_line = plain[execs[0]]
for required in (
    "--state /var/lib/gaudere/state.db",
    "--control-socket /tmp/gaudere-control.sock",
    "--local-activity-sidecar /var/lib/gaudere/local-activity-pulse.db",
    "--local-goose-model ",
    "--local-goose-model-sha256 ",
    "--local-goose-governance /var/lib/gaudere/goose-governance.db",
    "--local-goose-cycle-sidecar /var/lib/gaudere/local-goose-cycle.db",
):
    if required not in exec_line:
        raise SystemExit(f"installed Quadlet lacks required Local Goose setting: {required}")
if "--local-goose-cycle-stimulus-sidecar " in exec_line:
    raise SystemExit("installed Quadlet already has stimulus wiring")
for forbidden in ("--openai-model ", "--autonomous-pulse-provider", "--wake-intents"):
    if forbidden in exec_line:
        raise SystemExit(f"installed Quadlet contains forbidden provider authority: {forbidden.strip()}")
for required_line in (
    "Network=none",
    "ReadOnly=true",
    "ReadOnlyTmpfs=true",
    "NoNewPrivileges=true",
    "DropCapability=all",
):
    if required_line not in plain:
        raise SystemExit(f"installed Quadlet lost hardening: {required_line}")

image_index = images[0]
ending = "\n" if lines[image_index].endswith("\n") else ""
lines[image_index] = f"Image={candidate_id}{ending}"
exec_index = execs[0]
ending = "\n" if lines[exec_index].endswith("\n") else ""
lines[exec_index] = lines[exec_index].rstrip("\n") + (
    " --local-goose-cycle-stimulus-sidecar "
    "/var/lib/gaudere/local-goose-cycle-stimulus.db"
) + ending
destination.write_text("".join(lines), encoding="utf-8")

old = source.read_text(encoding="utf-8").splitlines()
new = destination.read_text(encoding="utf-8").splitlines()

def normalize(items, candidate):
    out = []
    for line in items:
        if line.startswith("Image="):
            out.append("Image=<allowed>")
        elif candidate and line.startswith("Exec="):
            marker = " --local-goose-cycle-stimulus-sidecar "
            out.append(line.split(marker, 1)[0] if marker in line else line)
        else:
            out.append(line)
    return out

if normalize(old, False) != normalize(new, True):
    raise SystemExit("deploy attempted an unauthorized Quadlet mutation")
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
    if [ "$service_stopped" = "0" ]         && [ "$state_mutated" = "0" ]         && [ "$profile_mutated" = "0" ]; then
        exit "$status"
    fi

    printf 'gaudere Local Goose stimulus deploy: recovery starting\n' >&2
    "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
    if [ "$profile_mutated" = "1" ]; then
        install -m 0600 "$previous_quadlet" "$target_quadlet" || true
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
    fi
    if [ "$state_mutated" = "1" ] && [ -n "$backup_archive" ]         && [ -f "$backup_archive" ]; then
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

printf '=== STOP / BACKUP / INITIALIZE EMPTY STIMULUS LEDGER ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ]     || fail "production service did not stop cleanly"

backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ]     || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"

init_output=$("$podman_command" run --rm --network none     --userns=keep-id --user "$host_uid:$host_gid"     --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --volume "$state_directory:/var/lib/gaudere:Z"     --entrypoint /usr/local/bin/gaudere-local-goose-cycle-stimulus-init     "$candidate_id"     --stimulus-sidecar /var/lib/gaudere/local-goose-cycle-stimulus.db)     || fail "bounded stimulus sidecar initialization failed"
state_mutated=1
printf '%s\n' "$init_output"
printf '%s\n' "$init_output" | grep -q '^LOCAL_GOOSE_STIMULUS_INIT=PASS$'     || fail "stimulus initializer did not report PASS"

[ -f "$stimulus_sidecar" ] && [ ! -L "$stimulus_sidecar" ]     || fail "stimulus sidecar was not created safely"
[ "$(stat -c '%a' "$stimulus_sidecar")" = "600" ]     || fail "stimulus sidecar mode is not 0600"
[ "$(stat -c '%u' "$stimulus_sidecar")" = "$host_uid" ]     || fail "stimulus sidecar owner differs from host user"
[ "$(sqlite3 -readonly "$stimulus_sidecar" 'PRAGMA user_version;')" = "1" ]     || fail "stimulus sidecar schema is not v1"
[ "$(sqlite3 -readonly "$stimulus_sidecar"     'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ]     || fail "stimulus sidecar is not empty after initialization"

install -m 0600 "$rendered_quadlet" "$target_quadlet"
profile_mutated=1
"$systemctl_command" --user daemon-reload

printf '=== START PRODUCTION WITH STIMULUS CODE WIRED BUT IDLE ===\n'
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ]     || fail "candidate production service did not become active"
[ "$(running_image)" = "$candidate_id_normalized" ]     || fail "running service image is not approved candidate"

sleep 2
[ "$(service_state)" = "active" ]     || fail "candidate production service did not remain active"

network_after=$("$podman_command" inspect gaudere-agent     --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "none" ]     || fail "production network changed from none"

provider_after=$(provider_total)
[ "$provider_after" = "$provider_before" ]     || fail "provider total changed during stimulus code deploy"

cursor_after=$(cycle_cursor)
[ "$cursor_after" = "$cursor_before" ]     || fail "production Local Goose cursor changed during code-only deploy"

tasks_after=$(cycle_task_count)
[ "$tasks_after" = "$tasks_before" ]     || fail "production Local Goose Task count changed during code-only deploy"

[ "$(sqlite3 -readonly "$stimulus_sidecar"     'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ]     || fail "code-only deploy accepted a stimulus unexpectedly"

grep -q -- '--local-goose-cycle-stimulus-sidecar /var/lib/gaudere/local-goose-cycle-stimulus.db'     "$target_quadlet"     || fail "installed Quadlet lacks bounded stimulus wiring"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents'     "$target_quadlet"     || fail "production profile unexpectedly gained provider authority"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'CANDIDATE_IMAGE=%s\n' "$candidate_id_normalized"
printf 'ROLLBACK_IMAGE=%s\n' "$previous_running_image"
printf 'NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'CYCLE_CURSOR=%s\n' "$cursor_after"
printf 'LOCAL_GOOSE_TASKS=%s\n' "$tasks_after"
printf 'STIMULI=0\n'
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_LOCAL_GOOSE_STIMULUS_CODE_DEPLOY=PASS\n'
