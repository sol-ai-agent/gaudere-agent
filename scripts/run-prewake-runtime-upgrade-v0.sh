#!/bin/sh
set -eu

# PREP ONLY until Bertrand gives a separate explicit production GO.
#
# Upgrade schema-v4 production to a current attributable Agent/Core image while
# WakeIntent remains disabled. This script never submits provider work and never
# accepts/revokes a WakeIntent.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(git -C "$script_directory/.." rev-parse --show-toplevel 2>/dev/null) || {
    printf 'gaudere prewake runtime upgrade: cannot resolve Git checkout\n' >&2
    exit 1
}

fail()
{
    printf 'gaudere prewake runtime upgrade: %s\n' "$*" >&2
    exit 1
}

normalize_image_id()
{
    value=$1
    case "$value" in
        sha256:*) digest=${value#sha256:} ;;
        *) digest=$value ;;
    esac
    case "$digest" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#digest}" -eq 64 ] || return 1
    printf 'sha256:%s\n' "$digest"
}

report_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
}

[ "$#" -eq 1 ] || fail "usage: $0 --execute-after-explicit-production-go"
[ "$1" = "--execute-after-explicit-production-go" ] \
    || fail "explicit production authorization argument is required"

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
test_mode=${GAUDERE_TEST_MODE:-0}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
container_name=${GAUDERE_CONTAINER:-gaudere-agent}
state_directory=${GAUDERE_STATE_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"}
representative_task=${GAUDERE_REPRESENTATIVE_TASK:-production-initiative-first}
frozen_previous_image_id=sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3
expected_previous_image_id=${GAUDERE_EXPECTED_PREVIOUS_IMAGE_ID:-$frozen_previous_image_id}
canonical_build="$repo_root/scripts/build-image.sh"
canonical_gate="$repo_root/scripts/validate-schema-v4-service-wake-off.sh"
canonical_control="$repo_root/scripts/control-service.sh"
build_script=${GAUDERE_PREWAKE_BUILD_SCRIPT:-$canonical_build}
wake_gate=${GAUDERE_PREWAKE_WAKE_OFF_GATE:-$canonical_gate}
control_script=${GAUDERE_PREWAKE_CONTROL_SCRIPT:-$canonical_control}

case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac
if [ "$test_mode" = "0" ]; then
    [ "$build_script" = "$canonical_build" ] \
        || fail "build-script override is restricted to synthetic test mode"
    [ "$wake_gate" = "$canonical_gate" ] \
        || fail "wake-gate override is restricted to synthetic test mode"
    [ "$control_script" = "$canonical_control" ] \
        || fail "control-script override is restricted to synthetic test mode"
    [ "$expected_previous_image_id" = "$frozen_previous_image_id" ] \
        || fail "previous production image override is restricted to synthetic test mode"
fi

for command in git sed tail cut; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
[ -x "$build_script" ] || fail "build script not executable: $build_script"
[ -f "$wake_gate" ] || fail "wake-off gate not found: $wake_gate"
[ -f "$control_script" ] || fail "control helper not found: $control_script"
[ -f "$repo_root/gaudere.ref" ] || fail "gaudere.ref is missing"
[ -f "$state_directory/state.db" ] || fail "production state database is missing"
expected_previous_image_id=$(normalize_image_id "$expected_previous_image_id") \
    || fail "expected previous production image ID is invalid"

agent_ref=$(git -C "$repo_root" rev-parse HEAD)
case "$agent_ref" in
    *[!0-9a-f]*|'') fail "Agent HEAD is not one hexadecimal commit" ;;
esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD must be 40 hexadecimal characters"
if [ -n "$(git -C "$repo_root" status --porcelain --untracked-files=normal)" ]; then
    fail "gaudere-agent checkout must be clean"
fi
core_ref=$(tr -d '\r\n' < "$repo_root/gaudere.ref")
case "$core_ref" in
    *[!0-9a-f]*|'') fail "gaudere.ref is not one hexadecimal commit" ;;
esac
[ "${#core_ref}" -eq 40 ] || fail "gaudere.ref must contain 40 hexadecimal characters"

service_state=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name must be active before the upgrade"
raw_previous_image_id=$("$podman_command" container inspect --format '{{.Image}}' "$container_name" 2>/dev/null) \
    || fail "cannot resolve currently running production image"
previous_image_id=$(normalize_image_id "$raw_previous_image_id") \
    || fail "currently running production image is not one immutable sha256 ID"
[ "$previous_image_id" = "$expected_previous_image_id" ] \
    || fail "running production image does not match the frozen pre-wake baseline"
"$podman_command" image exists "$previous_image_id" \
    || fail "frozen pre-wake rollback image is not retained locally"

before_budget=$(sh "$control_script" budget)
printf '%s\n' "$before_budget"
[ "$(report_value provider_enabled "$before_budget")" = "true" ] \
    || fail "provider capability is not enabled before upgrade"
[ "$(report_value total_used "$before_budget")" = "3" ] \
    || fail "pre-wake runtime upgrade requires exactly three durable provider consumptions"

before_task=$(sh "$control_script" task "$representative_task")
printf '%s\n' "$before_task"
printf '%s\n' "$before_task" | grep -qx 'status=succeeded' \
    || fail "representative historical Task is not succeeded"
printf '%s\n' "$before_task" | grep -qx \
    'result_metadata_content_type="application/vnd.gaudere.provider-usage+json"' \
    || fail "representative historical Task lacks provider usage metadata"

short_ref=$(printf '%s' "$agent_ref" | cut -c1-12)
candidate_tag=${GAUDERE_PREWAKE_IMAGE_TAG:-"localhost/gaudere-agent:prewake-$short_ref"}
printf 'status=AUTHORIZED_PREWAKE_UPGRADE_PREFLIGHT\n'
printf 'agent_ref=%s\n' "$agent_ref"
printf 'core_ref=%s\n' "$core_ref"
printf 'previous_image_id=%s\n' "$previous_image_id"
printf 'candidate_tag=%s\n' "$candidate_tag"
printf 'provider_total_before=3\n'
printf 'wake_capability_before=false\n'

GAUDERE_IMAGE_TAG="$candidate_tag" PODMAN="$podman_command" sh "$build_script"
raw_candidate_id=$("$podman_command" image inspect --format '{{.Id}}' "$candidate_tag" 2>/dev/null) \
    || fail "cannot inspect built candidate image"
candidate_id=$(normalize_image_id "$raw_candidate_id") \
    || fail "candidate image did not resolve to one immutable sha256 ID"
[ "$candidate_id" != "$previous_image_id" ] \
    || fail "candidate image unexpectedly equals the frozen production image"
printf 'candidate_image_id=%s\n' "$candidate_id"

# Prove the candidate actually contains the recovery observability that motivates
# this upgrade, without a state mount, secret, network or provider configuration.
control_usage=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    --entrypoint /usr/local/bin/gaudere-control \
    "$candidate_id" 2>&1 || true)
printf '%s\n' "$control_usage" | grep -q 'wake-status' \
    || fail "candidate image does not expose wake-status observability"

"$systemctl_command" --user stop "$service_name"
[ "$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)" = "inactive" ] \
    || fail "$service_name did not become inactive"

if ! gate_output=$(\
    GAUDERE_STATE_DIR="$state_directory" \
    GAUDERE_SERVICE_NAME="$service_name" \
    GAUDERE_RUNTIME_IMAGE="$candidate_tag" \
    GAUDERE_EXPECTED_RUNTIME_IMAGE_ID="$candidate_id" \
    GAUDERE_SCHEMA_V4_WAKE_OFF_AUTHORIZATION=AUTHORIZED_SCHEMA_V4_WAKE_OFF_GATE \
    GAUDERE_TEST_MODE="$test_mode" \
    SYSTEMCTL="$systemctl_command" PODMAN="$podman_command" \
        sh "$wake_gate" "$representative_task" 2>&1); then
    printf '%s\n' "$gate_output" >&2
    fail "schema-v4 wake-off candidate gate failed; service/profile state requires review"
fi
printf '%s\n' "$gate_output"
printf '%s\n' "$gate_output" | grep -q '^gaudere schema v4 wake-off service gate: PASS$' \
    || fail "candidate gate did not report PASS"
printf '%s\n' "$gate_output" | grep -q '^provider_effects=0$' \
    || fail "candidate gate did not prove zero provider effects"
printf '%s\n' "$gate_output" | grep -q '^wake_effects=0$' \
    || fail "candidate gate did not prove zero wake effects"
printf '%s\n' "$gate_output" | grep -q '^runtime_image_identity=PASS$' \
    || fail "candidate gate did not prove immutable running image identity"

[ "$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)" = "active" ] \
    || fail "$service_name is not active after candidate gate"
"$podman_command" image exists "$previous_image_id" \
    || fail "frozen rollback image disappeared after successful upgrade"

if wake_status=$(sh "$control_script" wake-status 2>&1); then
    printf '%s\n' "$wake_status" >&2
    fail "wake-status unexpectedly succeeded while WakeIntent should remain disabled"
fi
printf '%s\n' "$wake_status"
printf '%s\n' "$wake_status" | grep -q \
    'explicit wake capability is not enabled in this service' \
    || fail "new wake-status surface did not prove WakeIntent disabled"

after_budget=$(sh "$control_script" budget)
printf '%s\n' "$after_budget"
[ "$(report_value provider_enabled "$after_budget")" = "true" ] \
    || fail "provider capability is not enabled after upgrade"
[ "$(report_value total_used "$after_budget")" = "3" ] \
    || fail "provider durable budget changed during upgrade"

after_task=$(sh "$control_script" task "$representative_task")
printf '%s\n' "$after_task"
printf '%s\n' "$after_task" | grep -qx 'status=succeeded' \
    || fail "representative historical Task changed during upgrade"

printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'wake_capability_active=false\n'
printf 'wake_status_surface=present\n'
printf 'service_final=active\n'
printf 'agent_ref=%s\n' "$agent_ref"
printf 'core_ref=%s\n' "$core_ref"
printf 'candidate_image_id=%s\n' "$candidate_id"
printf 'rollback_image_id=%s\n' "$previous_image_id"
printf 'rollback_image_retained=true\n'
printf 'gaudere prewake runtime upgrade: PASS\n'
