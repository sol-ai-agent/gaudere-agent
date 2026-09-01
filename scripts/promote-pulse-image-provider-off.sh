#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
image=${GAUDERE_IMAGE:-}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-}
pulse_container_path=${GAUDERE_PULSE_SIDECAR_CONTAINER_PATH:-/var/lib/gaudere/autonomous-cognition-pulse.db}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
provenance_verifier="$script_directory/verify-image-provenance.sh"

fail()
{
    printf 'gaudere provider-off pulse promotion: %s\n' "$*" >&2
    exit 1
}

require_service_stopped()
{
    service_state=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
    case "$service_state" in
        active|activating|reloading)
            fail "$service_name must be stopped before image promotion"
            ;;
    esac
}

[ -n "$image" ] || fail "GAUDERE_IMAGE is required"
[ -n "$expected_agent_ref" ] || fail "GAUDERE_EXPECTED_AGENT_REF is required"
[ -n "$expected_core_ref" ] || fail "GAUDERE_EXPECTED_CORE_REF is required"
[ -x "$provenance_verifier" ] \
    || fail "image provenance verifier is required: $provenance_verifier"

for command in "$podman_command" "$systemctl_command" python3 install mktemp rm grep sed wc; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done

[ -f "$target_quadlet" ] && [ ! -L "$target_quadlet" ] \
    || fail "target Quadlet must be an existing regular non-symlink file: $target_quadlet"

require_service_stopped

# This promotion is deliberately narrower than a profile install: retain every
# production setting and replace only Image=. Refuse a profile that is not the
# already-promoted provider-free autonomous pulse service.
python3 - "$target_quadlet" "$pulse_container_path" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
pulse_path = sys.argv[2]
text = path.read_text(encoding="utf-8")
lines = text.splitlines()
images = [line for line in lines if line.startswith("Image=")]
execs = [line for line in lines if line.startswith("Exec=")]
if len(images) != 1:
    raise SystemExit("target Quadlet must contain exactly one Image= line")
if len(execs) != 1:
    raise SystemExit("target Quadlet must contain exactly one Exec= line")
exec_line = execs[0]
needle = f"--autonomous-pulse-sidecar {pulse_path}"
if needle not in exec_line:
    raise SystemExit("target Quadlet is not the expected provider-free pulse profile")
if "--autonomous-pulse-provider" in exec_line:
    raise SystemExit("target Quadlet already grants autonomous provider authority")
if "--wake-intents" in exec_line:
    raise SystemExit("target Quadlet unexpectedly enables WakeIntent")
if "--openai-model " not in exec_line:
    raise SystemExit("target Quadlet lost its bounded OpenAI service configuration")
PY

provenance_output=$(PODMAN="$podman_command" sh "$provenance_verifier" \
    "$image" "$expected_agent_ref" "$expected_core_ref") \
    || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
image_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p')
case "$image_id" in
    sha256:*) ;;
    *) fail "provenance verifier did not return one immutable image ID" ;;
esac
[ "$(printf '%s\n' "$image_id" | wc -l)" -eq 1 ] \
    || fail "provenance verifier returned ambiguous image identity"

# Prove the candidate has the newly merged provider-capable binary, but do so in
# an isolated help-only container with no state, secret or network. Capability in
# the image is not authority in the service profile.
help_output=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    "$image_id" --help 2>&1) \
    || fail "candidate gaudere-agent --help probe failed"
printf '%s\n' "$help_output" | grep -q -- '--autonomous-pulse-provider' \
    || fail "candidate image predates autonomous pulse provider capability"

rendered_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-provider-off-quadlet.XXXXXX")
previous_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-provider-off-previous.XXXXXX")
install -m 0600 "$target_quadlet" "$previous_quadlet"

cleanup()
{
    rm -f -- "$rendered_quadlet" "$previous_quadlet"
}
trap cleanup EXIT
trap 'cleanup; exit 1' HUP INT TERM

python3 - "$target_quadlet" "$rendered_quadlet" "$image_id" "$pulse_container_path" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
image_id = sys.argv[3]
pulse_path = sys.argv[4]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
indexes = [i for i, line in enumerate(lines) if line.startswith("Image=")]
if len(indexes) != 1:
    raise SystemExit("target Quadlet must contain exactly one Image= line")
index = indexes[0]
ending = "\n" if lines[index].endswith("\n") else ""
lines[index] = f"Image={image_id}{ending}"
rendered = "".join(lines)
execs = [line for line in rendered.splitlines() if line.startswith("Exec=")]
if len(execs) != 1:
    raise SystemExit("rendered Quadlet must contain exactly one Exec= line")
if f"--autonomous-pulse-sidecar {pulse_path}" not in execs[0]:
    raise SystemExit("rendered Quadlet lost autonomous pulse sidecar")
if "--autonomous-pulse-provider" in execs[0]:
    raise SystemExit("rendered Quadlet unexpectedly grants autonomous provider authority")
if "--wake-intents" in execs[0]:
    raise SystemExit("rendered Quadlet unexpectedly enables WakeIntent")
destination.write_text(rendered, encoding="utf-8")
PY

# Compare everything except Image= before touching the live profile. This makes
# the intended mutation mechanically narrow rather than a convention.
python3 - "$target_quadlet" "$rendered_quadlet" <<'PY'
import pathlib
import sys

def without_image(path):
    return [line for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines(keepends=True)
            if not line.startswith("Image=")]

if without_image(sys.argv[1]) != without_image(sys.argv[2]):
    raise SystemExit("provider-off promotion attempted to change more than Image=")
PY

# Close the validation-to-install window as much as possible without starting or
# owning the service here. B10 remains responsible for the outer stopped-state
# backup/start/restart sequence.
require_service_stopped
install -m 0600 "$rendered_quadlet" "$target_quadlet"
if ! "$systemctl_command" --user daemon-reload; then
    install -m 0600 "$previous_quadlet" "$target_quadlet" || true
    "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
    fail "daemon-reload failed; previous Quadlet restored"
fi

cleanup
trap - EXIT HUP INT TERM

printf 'gaudere provider-off pulse promotion: quadlet=%s\n' "$target_quadlet"
printf 'gaudere provider-off pulse promotion: runtime_image_id=%s\n' "$image_id"
printf 'gaudere provider-off pulse promotion: pulse_sidecar=%s\n' "$pulse_container_path"
printf 'gaudere provider-off pulse promotion: autonomous_provider_authority=OFF\n'
printf 'gaudere provider-off pulse promotion: wake_intent_authority=OFF\n'
printf 'gaudere provider-off pulse promotion: service remains stopped\n'
