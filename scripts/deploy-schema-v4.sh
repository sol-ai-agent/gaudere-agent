#!/bin/sh
set -eu

# PREP ONLY / NOT AUTHORIZED FOR PRODUCTION.
#
# Public schema-v4 staging entrypoint. It consumes the reviewed image-provenance
# contract from issue #50 before delegating to the state-only staging engine.
# It never starts/stops a service, changes an installed profile, mounts a secret,
# calls a provider, consumes a provider permit, or creates a real WakeIntent.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backup_script="$script_directory/backup-state.sh"
default_provenance_validator="$script_directory/validate-schema-v4-image-provenance.sh"
default_stage_script="$script_directory/deploy-schema-v4-stage-internal.sh"
provenance_validator=${GAUDERE_SCHEMA_V4_IMAGE_PROVENANCE_VALIDATOR:-$default_provenance_validator}
stage_script=${GAUDERE_SCHEMA_V4_STAGE_SCRIPT:-$default_stage_script}
systemctl_command=${SYSTEMCTL:-systemctl}
test_mode=${GAUDERE_TEST_MODE:-0}
agent_bin=${GAUDERE_AGENT_BIN:-}
state_directory=${GAUDERE_STATE_DIR:-}
backup_directory=${GAUDERE_BACKUP_DIR:-}
service_name=${GAUDERE_SERVICE_NAME:-}
candidate_image=${GAUDERE_CANDIDATE_IMAGE:-}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-}
expected_candidate_id=${GAUDERE_EXPECTED_CANDIDATE_ID:-}
rollback_image=${GAUDERE_ROLLBACK_IMAGE:-}
expected_rollback_id=${GAUDERE_EXPECTED_ROLLBACK_ID:-}

fail()
{
    printf 'gaudere schema v4 proven deployment: %s\n' "$*" >&2
    exit 1
}

[ "$#" -eq 0 ] || fail "usage: $0"
case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac

[ -n "$state_directory" ] || fail "GAUDERE_STATE_DIR must be set explicitly"
[ -n "$backup_directory" ] || fail "GAUDERE_BACKUP_DIR must be set explicitly"
[ -n "$service_name" ] || fail "GAUDERE_SERVICE_NAME must be set explicitly"
[ -f "$backup_script" ] || fail "backup script not found: $backup_script"
[ -f "$stage_script" ] || fail "stage script not found: $stage_script"
command -v "$systemctl_command" >/dev/null 2>&1 \
    || fail "required command not found: $systemctl_command"
command -v flock >/dev/null 2>&1 || fail "required command not found: flock"
command -v grep >/dev/null 2>&1 || fail "required command not found: grep"

if [ "$test_mode" = "0" ]; then
    [ "$provenance_validator" = "$default_provenance_validator" ] \
        || fail "provenance validator override is restricted to synthetic test mode"
    [ "$stage_script" = "$default_stage_script" ] \
        || fail "stage script override is restricted to synthetic test mode"
    [ -x "$provenance_validator" ] \
        || fail "image provenance validator not found: $provenance_validator"
    [ -n "$candidate_image" ] \
        || fail "GAUDERE_CANDIDATE_IMAGE must name the reviewed candidate tag"
    [ -n "$expected_agent_ref" ] \
        || fail "GAUDERE_EXPECTED_AGENT_REF must be set"
    [ -n "$expected_core_ref" ] \
        || fail "GAUDERE_EXPECTED_CORE_REF must be set"
    [ -n "$expected_candidate_id" ] \
        || fail "GAUDERE_EXPECTED_CANDIDATE_ID must be set"
    [ -n "$rollback_image" ] \
        || fail "GAUDERE_ROLLBACK_IMAGE must name the retained rollback tag"
    [ -n "$expected_rollback_id" ] \
        || fail "GAUDERE_EXPECTED_ROLLBACK_ID must be set"
else
    # The pre-existing staged-state failure-injection test uses a direct synthetic
    # Agent binary and fake systemctl. Its legacy path may skip Podman provenance
    # only inside its dedicated mktemp tree; GAUDERE_TEST_MODE=1 alone is never a
    # provenance bypass for an arbitrary state path.
    if [ -z "${GAUDERE_SCHEMA_V4_IMAGE_PROVENANCE_VALIDATOR:-}" ]; then
        [ -n "$agent_bin" ] \
            || fail "test-mode provenance bypass requires GAUDERE_AGENT_BIN"
        [ "$systemctl_command" != "systemctl" ] \
            || fail "test-mode provenance bypass requires synthetic systemctl"
        case "$state_directory" in
            "${TMPDIR:-/tmp}"/gaudere-schema-v4-deploy.*/*) ;;
            *) fail "test-mode provenance bypass is restricted to the staged-deployment fixture" ;;
        esac
        provenance_validator=
        candidate_image=${candidate_image:-synthetic-candidate}
        expected_agent_ref=${expected_agent_ref:-1111111111111111111111111111111111111111}
        expected_core_ref=${expected_core_ref:-2222222222222222222222222222222222222222}
        expected_candidate_id=${expected_candidate_id:-sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}
        rollback_image=${rollback_image:-synthetic-rollback}
        expected_rollback_id=${expected_rollback_id:-sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb}
    else
        [ -x "$provenance_validator" ] \
            || fail "synthetic provenance validator is not executable"
    fi
fi

[ -d "$state_directory" ] || fail "state directory not found: $state_directory"
[ -f "$state_directory/state.db" ] \
    || fail "state database not found: $state_directory/state.db"

observed_service_state=$(
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
)
[ "$observed_service_state" = "inactive" ] \
    || fail "$service_name must report exactly inactive (found ${observed_service_state:-unknown})"

exec 9>>"$state_directory/state.db.lock"
chmod 600 "$state_directory/state.db.lock" 2>/dev/null || true
if ! flock -n 9; then
    fail "state database is currently owned"
fi
flock -u 9
exec 9>&-

printf 'status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION\n'
printf 'provenance_contract=gaudere.schema-v4-image-provenance.v1\n'
printf '\n==> create fresh provider-free backup for the image-provenance gate\n'
if ! provenance_backup=$(GAUDERE_STATE_DIR="$state_directory" \
        GAUDERE_BACKUP_DIR="$backup_directory" sh "$backup_script"); then
    fail "provenance backup creation failed"
fi
[ -f "$provenance_backup" ] || fail "provenance backup was not created"
[ -f "$provenance_backup.sha256" ] \
    || fail "provenance backup checksum was not created"
printf 'provenance_backup=%s\n' "$provenance_backup"

printf '\n==> consume reviewed candidate/rollback provenance contract\n'
if [ -z "$provenance_validator" ]; then
    provenance_output='status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION
provider_effects=0
wake_effects=0
production_state_touched=false
gaudere schema v4 image provenance validation: PASS'
else
    if ! provenance_output=$(\
        GAUDERE_CANDIDATE_IMAGE="$candidate_image" \
        GAUDERE_EXPECTED_AGENT_REF="$expected_agent_ref" \
        GAUDERE_EXPECTED_CORE_REF="$expected_core_ref" \
        GAUDERE_EXPECTED_CANDIDATE_ID="$expected_candidate_id" \
        GAUDERE_ROLLBACK_IMAGE="$rollback_image" \
        GAUDERE_EXPECTED_ROLLBACK_ID="$expected_rollback_id" \
        GAUDERE_TEST_MODE="$test_mode" \
            sh "$provenance_validator" "$provenance_backup" 2>&1); then
        printf '%s\n' "$provenance_output" >&2
        fail "image provenance contract failed"
    fi
fi
printf '%s\n' "$provenance_output"
printf '%s\n' "$provenance_output" | grep -q \
    '^gaudere schema v4 image provenance validation: PASS$' \
    || fail "image provenance validator did not report PASS"
printf '%s\n' "$provenance_output" | grep -q '^provider_effects=0$' \
    || fail "image provenance gate did not prove zero provider effects"
printf '%s\n' "$provenance_output" | grep -q '^wake_effects=0$' \
    || fail "image provenance gate did not prove zero wake effects"
printf '%s\n' "$provenance_output" | grep -q '^production_state_touched=false$' \
    || fail "image provenance gate did not prove production state untouched"

printf 'candidate_image=%s\n' "$candidate_image"
printf 'candidate_image_id=%s\n' "$expected_candidate_id"
printf 'rollback_image=%s\n' "$rollback_image"
printf 'rollback_image_id=%s\n' "$expected_rollback_id"
printf 'image_provenance_gate=PASS\n'

printf '\n==> run state-only staging engine on the immutable candidate image ID\n'
# Never delegate the mutable candidate tag. The #50 gate already proved that tag
# against the exact ID; the state engine receives only that immutable ID.
GAUDERE_IMAGE="$expected_candidate_id" sh "$stage_script"
