#!/bin/sh
set -eu

authorization=${GAUDERE_LOCAL_GOOSE_CYCLE_PRODUCTION_AUTHORIZATION:-}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_previous_image=${GAUDERE_EXPECTED_PREVIOUS_IMAGE:-9bae154c1c47ea4351e452851874f65c84dc132970f7307b6cf4cde40d61010e}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87}
podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
activity_sidecar="$state_directory/local-activity-pulse.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
governance_sidecar="$state_directory/goose-governance.db"
goose_root=${GAUDERE_GOOSE_ROOT:-"$data_home/gaudere/goose"}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
backup_script="$script_directory/backup-state.sh"
build_script="$script_directory/build-image.sh"
provenance_script="$script_directory/verify-image-provenance.sh"

fail()
{
    printf 'gaudere Local Goose cycle production activation: FAIL: %s\n' "$*" >&2
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

[ "$authorization" = "AUTHORIZED_LOCAL_GOOSE_CYCLE_PRODUCTION" ]     || fail "explicit production authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3         sha256sum tar install mkdir mktemp mv rm sed grep awk sleep date; do
    command -v "$command" >/dev/null 2>&1         || fail "required command not found: $command"
done
[ -x "$backup_script" ] || fail "backup script is missing"
[ -x "$build_script" ] || fail "image build script is missing"
[ -x "$provenance_script" ] || fail "image provenance verifier is missing"
[ -f "$state_database" ] && [ ! -L "$state_database" ]     || fail "production state database is missing or unsafe"
[ -f "$activity_sidecar" ] && [ ! -L "$activity_sidecar" ]     || fail "local activity sidecar is missing or unsafe"
[ -f "$governance_sidecar" ] && [ ! -L "$governance_sidecar" ]     || fail "Goose governance sidecar is missing or unsafe"
[ -d "$goose_root" ] && [ ! -L "$goose_root" ]     || fail "Goose root is missing or unsafe"
[ -f "$target_quadlet" ] && [ ! -L "$target_quadlet" ]     || fail "installed Quadlet is missing or unsafe"
[ ! -e "$cycle_sidecar" ]     || fail "production Local Goose cycle sidecar already exists; refusing bootstrap replay"

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ]     || fail "script must belong to the gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ]     || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ]     || fail "gaudere-agent checkout must be clean"
agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")

[ "$(service_state)" = "active" ] || fail "production service must be active before activation"
previous_running_image=$(running_image)
[ "$previous_running_image" = "$expected_previous_image" ]     || fail "production image differs from expected pre-activation image"
provider_before=$(provider_total)
[ "$provider_before" = "$expected_provider_total" ]     || fail "provider total before activation is $provider_before, expected $expected_provider_total"
preexisting_cycles=$(sqlite3 -readonly "$state_database"     "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
[ "$preexisting_cycles" = "0" ]     || fail "production state already contains Local Goose cycle Tasks"

transition_root="$data_home/gaudere/.local-goose-cycle-activation"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
previous_quadlet="$workspace/gaudere-agent.container.before"
rendered_quadlet="$workspace/gaudere-agent.container.candidate"
install -m 0600 "$target_quadlet" "$previous_quadlet"

candidate_tag="localhost/gaudere-agent:local-goose-cycle-prod-$agent_ref"
printf '=== BUILD PRODUCTION CANDIDATE ===\n'
GAUDERE_IMAGE_TAG="$candidate_tag" sh "$build_script"

provenance_output=$(PODMAN="$podman_command" sh "$provenance_script"     "$candidate_tag" "$agent_ref" "$core_ref")     || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
candidate_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p' | tail -n 1)
case "$candidate_id" in
    sha256:*) ;;
    *) fail "provenance verifier did not return immutable sha256 image ID" ;;
esac
candidate_id_normalized=$(normalize_image "$candidate_id")

"$podman_command" run --rm --network none --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --entrypoint /usr/local/bin/gaudere-agent "$candidate_id" --help     | grep -q -- '--local-goose-cycle-sidecar PATH'     || fail "candidate main service lacks Local Goose cycle capability"
for binary in gaudere-local-goose-cycle-seed gaudere-local-goose-cycle-activate; do
    "$podman_command" run --rm --network none --read-only --read-only-tmpfs         --security-opt=no-new-privileges --cap-drop=all         --entrypoint /usr/bin/test "$candidate_id"         -x "/usr/local/bin/$binary"         || fail "candidate image lacks $binary"
done

python3 - "$target_quadlet" "$rendered_quadlet" "$candidate_id"     "$model_id" "$model_sha256" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
candidate_id, model_id, model_sha = sys.argv[3:]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
plain = [line.rstrip("\n") for line in lines]
images = [i for i, line in enumerate(plain) if line.startswith("Image=")]
execs = [i for i, line in enumerate(plain) if line.startswith("Exec=")]
if len(images) != 1 or len(execs) != 1:
    raise SystemExit("installed Quadlet must contain exactly one Image= and Exec=")
exec_line = plain[execs[0]]
required = (
    "--state /var/lib/gaudere/state.db",
    "--control-socket /tmp/gaudere-control.sock",
    "--local-activity-sidecar /var/lib/gaudere/local-activity-pulse.db",
    f"--local-goose-model /{model_id}",
    f"--local-goose-model-sha256 {model_sha}",
    "--local-goose-governance /var/lib/gaudere/goose-governance.db",
)
for item in required:
    if item not in exec_line:
        raise SystemExit(f"installed Quadlet lacks required provider-free Local Goose setting: {item}")
for forbidden in (
    "--local-goose-cycle-sidecar ",
    "--openai-model ",
    "--autonomous-pulse-provider",
    "--wake-intents",
):
    if forbidden in exec_line:
        raise SystemExit(f"installed Quadlet contains forbidden/pre-existing authority: {forbidden.strip()}")
for required_line in (
    "Network=none",
    "ReadOnly=true",
    "ReadOnlyTmpfs=true",
    "NoNewPrivileges=true",
    "DropCapability=all",
):
    if required_line not in plain:
        raise SystemExit(f"installed Quadlet lost required hardening: {required_line}")
if not any(line.startswith("Memory=") for line in plain):
    raise SystemExit("installed Quadlet lacks bounded memory")
volumes = [line for line in plain if line.startswith("Volume=")]
for destination_path in (
    ":/var/lib/gaudere:Z",
    ":/var/lib/gaudere/goose:rw,Z",
    ":/var/lib/gaudere/goose/cache/huggingface:ro",
):
    if not any(destination_path in line for line in volumes):
        raise SystemExit(f"installed Quadlet lacks required mount: {destination_path}")

image_index = images[0]
ending = "\n" if lines[image_index].endswith("\n") else ""
lines[image_index] = f"Image={candidate_id}{ending}"
exec_index = execs[0]
ending = "\n" if lines[exec_index].endswith("\n") else ""
lines[exec_index] = lines[exec_index].rstrip("\n") + (
    " --local-goose-cycle-sidecar /var/lib/gaudere/local-goose-cycle.db"
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
            marker = " --local-goose-cycle-sidecar "
            out.append(line.split(marker, 1)[0] if marker in line else line)
        else:
            out.append(line)
    return out

if normalize(old, False) != normalize(new, True):
    raise SystemExit("activation attempted an unauthorized Quadlet mutation")
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
    if [ "$service_stopped" = "0" ] && [ "$state_mutated" = "0" ]             && [ "$profile_mutated" = "0" ]; then
        exit "$status"
    fi

    printf 'gaudere Local Goose cycle production activation: recovery starting\n' >&2
    "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true

    if [ "$profile_mutated" = "1" ]; then
        install -m 0600 "$previous_quadlet" "$target_quadlet" || true
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
    fi

    if [ "$state_mutated" = "1" ] && [ -n "$backup_archive" ]             && [ -f "$backup_archive" ]; then
        failed_state="$workspace/failed-state"
        if [ ! -e "$failed_state" ]; then
            mv "$state_directory" "$failed_state" || true
            mkdir -p -m 0700 "$state_directory" || true
            tar -xzf "$backup_archive" -C "$state_directory" || true
        fi
    fi

    "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
    restored_state=$(service_state)
    restored_image=$(running_image)
    restored_provider=$(provider_total 2>/dev/null || true)
    printf 'recovery_service=%s\n' "$restored_state" >&2
    printf 'recovery_image=%s\n' "$restored_image" >&2
    printf 'recovery_provider_total=%s\n' "$restored_provider" >&2
    printf 'recovery_workspace=%s\n' "$workspace" >&2
    exit "$status"
}
trap recover EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '\n=== STOP / BACKUP / EXPLICIT BOOTSTRAP ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly"

backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ]     || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"

seed_output=$("$podman_command" run --rm --network none     --userns=keep-id --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --volume "$state_directory:/var/lib/gaudere:Z"     --entrypoint /usr/local/bin/gaudere-local-goose-cycle-seed     "$candidate_id"     --state /var/lib/gaudere/state.db     --activity-sidecar /var/lib/gaudere/local-activity-pulse.db     --cycle-sidecar /var/lib/gaudere/local-goose-cycle.db)
state_mutated=1
printf '%s\n' "$seed_output"
printf '%s\n' "$seed_output" | grep -q '"state":"dormant"'     || fail "explicit seed did not produce dormant state"

activate_output=$("$podman_command" run --rm --network none     --userns=keep-id --read-only --read-only-tmpfs     --security-opt=no-new-privileges --cap-drop=all     --volume "$state_directory:/var/lib/gaudere:Z"     --entrypoint /usr/local/bin/gaudere-local-goose-cycle-activate     "$candidate_id"     --state /var/lib/gaudere/state.db     --cycle-sidecar /var/lib/gaudere/local-goose-cycle.db)
printf '%s\n' "$activate_output"
printf '%s\n' "$activate_output" | grep -q '"state":"scheduled"'     || fail "explicit activation did not schedule generation 1"

install -m 0600 "$rendered_quadlet" "$target_quadlet"
profile_mutated=1
"$systemctl_command" --user daemon-reload

printf '\n=== START PRODUCTION LOCAL GOOSE CYCLE ===\n'
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "candidate production service did not become active"
[ "$(running_image)" = "$candidate_id_normalized" ]     || fail "running service image is not the approved candidate"
network_mode=$("$podman_command" inspect gaudere-agent     --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_mode" = "none" ] || fail "production container network is not none"

settled=0
attempt=0
while [ "$attempt" -lt 300 ]; do
    cursor=$(sqlite3 -readonly "$cycle_sidecar"         "SELECT generation||'|'||state||'|'||COALESCE(predecessor_task_id,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';"         2>/dev/null || true)
    generation=$(printf '%s\n' "$cursor" | awk -F'|' '{print $1}')
    cycle_state=$(printf '%s\n' "$cursor" | awk -F'|' '{print $2}')
    predecessor=$(printf '%s\n' "$cursor" | awk -F'|' '{print $3}')
    if [ "$cycle_state" = "3" ]; then
        fail "production Local Goose cycle entered blocked state"
    fi
    case "$generation" in
        ''|*[!0-9]*) ;;
        *)
            if [ "$generation" -ge 2 ] && [ -n "$predecessor" ]; then
                settled=1
                break
            fi
            ;;
    esac
    [ "$(service_state)" = "active" ]         || fail "production service stopped while waiting for first cycle"
    attempt=$((attempt + 1))
    sleep 2
done
[ "$settled" = "1" ] || fail "first production Local Goose cycle did not settle within 10 minutes"

first_task=$(sqlite3 -readonly "$state_database"     "SELECT id||'|'||status||'|'||attempts_started FROM tasks WHERE kind='cognition.local-goose-cycle.v1' ORDER BY rowid ASC LIMIT 1;")
task_id=$(printf '%s\n' "$first_task" | awk -F'|' '{print $1}')
task_status=$(printf '%s\n' "$first_task" | awk -F'|' '{print $2}')
task_attempts=$(printf '%s\n' "$first_task" | awk -F'|' '{print $3}')
[ -n "$task_id" ] && [ "$task_status" = "3" ] && [ "$task_attempts" = "1" ]     || fail "first production Local Goose cycle Task is not one clean success"

provider_after=$(provider_total)
[ "$provider_after" = "$expected_provider_total" ]     || fail "provider total changed during Local Goose cycle activation"
[ "$(service_state)" = "active" ] || fail "production service is not active after proof"
[ "$(running_image)" = "$candidate_id_normalized" ]     || fail "production image changed during proof"
grep -q -- '--local-goose-cycle-sidecar /var/lib/gaudere/local-goose-cycle.db'     "$target_quadlet" || fail "installed Quadlet lost Local Goose cycle wiring"
! grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents'     "$target_quadlet" || fail "production profile unexpectedly gained provider/external authority"

committed=1
trap - EXIT HUP INT TERM

printf '\n=== PRODUCTION PROOF ===\n'
printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'CANDIDATE_IMAGE=%s\n' "$candidate_id_normalized"
printf 'ROLLBACK_IMAGE=%s\n' "$previous_running_image"
printf 'MODEL_ID=%s\n' "$model_id"
printf 'MODEL_SHA256=%s\n' "$model_sha256"
printf 'CYCLE_TASK=%s\n' "$task_id"
printf 'CYCLE_GENERATION=%s\n' "$generation"
printf 'CYCLE_STATE=%s\n' "$cycle_state"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'NETWORK=%s\n' "$network_mode"
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_LOCAL_GOOSE_CYCLE_PRODUCTION=PASS\n'
