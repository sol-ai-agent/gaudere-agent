#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
candidate_image=${GAUDERE_CANDIDATE_IMAGE:-}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-}
expected_candidate_id=${GAUDERE_EXPECTED_CANDIDATE_ID:-}
rollback_image=${GAUDERE_ROLLBACK_IMAGE:-}
expected_rollback_id=${GAUDERE_EXPECTED_ROLLBACK_ID:-}
agent_bin=${GAUDERE_AGENT_BIN:-}
test_mode=${GAUDERE_TEST_MODE:-0}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
provenance_verifier="$script_directory/verify-image-provenance.sh"
default_copy_validator="$script_directory/validate-schema-v4-migration-copy.sh"
copy_validator=${GAUDERE_SCHEMA_V4_COPY_VALIDATOR:-$default_copy_validator}
workspace=

fail()
{
    printf 'gaudere schema v4 image proof: %s\n' "$*" >&2
    exit 1
}

valid_image_ref()
{
    value=$1
    [ -n "$value" ] || return 1
    case "$value" in
        *[!A-Za-z0-9._:/@-]*) return 1 ;;
    esac
}

valid_image_id()
{
    value=$1
    case "$value" in
        sha256:*) digest=${value#sha256:} ;;
        *) return 1 ;;
    esac
    case "$digest" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#digest}" -eq 64 ]
}

resolve_image_id()
{
    reference=$1
    "$podman_command" image exists "$reference" \
        || fail "image does not exist: $reference"
    resolved=$("$podman_command" image inspect --format '{{.Id}}' "$reference") \
        || fail "cannot resolve image ID: $reference"
    valid_image_id "$resolved" \
        || fail "image did not resolve to one full sha256 ID: $reference"
    printf '%s\n' "$resolved"
}

cleanup()
{
    [ -z "$workspace" ] || rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

[ "$#" -ge 1 ] && [ "$#" -le 2 ] \
    || fail "usage: $0 BACKUP_ARCHIVE [TASK_ID]"
archive=$1
task_id=${2:-}

case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac
if [ "$copy_validator" != "$default_copy_validator" ] && [ "$test_mode" != "1" ]; then
    fail "GAUDERE_SCHEMA_V4_COPY_VALIDATOR override is restricted to synthetic test mode"
fi
if [ -n "$agent_bin" ] && [ "$test_mode" != "1" ]; then
    fail "GAUDERE_AGENT_BIN is restricted to synthetic test mode"
fi

valid_image_ref "$candidate_image" \
    || fail "GAUDERE_CANDIDATE_IMAGE must name an explicit candidate image"
valid_image_ref "$rollback_image" \
    || fail "GAUDERE_ROLLBACK_IMAGE must name an explicit rollback image"
[ "$candidate_image" != "$rollback_image" ] \
    || fail "candidate and rollback image references must differ"
valid_image_id "$expected_candidate_id" \
    || fail "GAUDERE_EXPECTED_CANDIDATE_ID must be one full sha256 image ID"
valid_image_id "$expected_rollback_id" \
    || fail "GAUDERE_EXPECTED_ROLLBACK_ID must be one full sha256 image ID"
[ -f "$archive" ] || fail "backup archive not found: $archive"
[ ! -L "$archive" ] || fail "backup archive must not be a symbolic link"
[ -x "$provenance_verifier" ] \
    || fail "image provenance verifier not found: $provenance_verifier"
[ -x "$copy_validator" ] \
    || fail "schema-v4 copy validator not found: $copy_validator"

for command in cat mktemp rm sed; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 \
    || fail "required command not found: $podman_command"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-image-proof.XXXXXX")

printf 'status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION\n'
printf '\n==> verify candidate image ID and Agent/Core provenance\n'
PODMAN="$podman_command" sh "$provenance_verifier" \
    "$candidate_image" "$expected_agent_ref" "$expected_core_ref" \
    "$expected_candidate_id" > "$workspace/candidate.before"
cat "$workspace/candidate.before"

printf '\n==> verify retained pre-v4 rollback tag and immutable ID\n'
rollback_id_before=$(resolve_image_id "$rollback_image")
[ "$rollback_id_before" = "$expected_rollback_id" ] \
    || fail "rollback image ID mismatch for $rollback_image"
printf 'rollback_image=%s\n' "$rollback_image"
printf 'rollback_image_id=%s\n' "$rollback_id_before"
printf 'rollback_identity=PASS\n'

printf '\n==> prove the exact candidate on disposable schema-v3 and schema-v4 copies\n'
if [ -n "$agent_bin" ]; then
    PODMAN="$podman_command" \
    GAUDERE_IMAGE="$candidate_image" \
    GAUDERE_AGENT_BIN="$agent_bin" \
        sh "$copy_validator" "$archive" "$task_id"
else
    PODMAN="$podman_command" \
    GAUDERE_IMAGE="$candidate_image" \
        sh "$copy_validator" "$archive" "$task_id"
fi

printf '\n==> reject candidate or rollback tag drift after the disposable proof\n'
PODMAN="$podman_command" sh "$provenance_verifier" \
    "$candidate_image" "$expected_agent_ref" "$expected_core_ref" \
    "$expected_candidate_id" > "$workspace/candidate.after"
candidate_id_after=$(sed -n 's/^image_id=//p' "$workspace/candidate.after")
[ "$candidate_id_after" = "$expected_candidate_id" ] \
    || fail "candidate image drifted during the disposable proof"
rollback_id_after=$(resolve_image_id "$rollback_image")
[ "$rollback_id_after" = "$expected_rollback_id" ] \
    || fail "rollback image drifted during the disposable proof"
printf 'candidate_image_id_after=%s\n' "$candidate_id_after"
printf 'rollback_image_id_after=%s\n' "$rollback_id_after"
printf 'image_tag_drift=NONE\n'

printf '\n==> schema-v4 candidate image provenance proof complete\n'
printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'production_state_touched=false\n'
printf 'gaudere schema v4 image provenance validation: PASS\n'
