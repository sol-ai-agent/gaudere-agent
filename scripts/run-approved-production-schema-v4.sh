#!/bin/sh
set -eu

# PREP ONLY until Bertrand gives a separate explicit production GO.
#
# This wrapper freezes the already-proved Fedora candidate/rollback identities and
# the reviewed host-side transaction helpers into one operator entrypoint. Merely
# running the script without the exact authorization argument is a no-op failure.
# It submits no provider work and never enables WakeIntent.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(git -C "$script_directory/.." rev-parse --show-toplevel 2>/dev/null) \
    || { printf 'gaudere approved production v4: cannot resolve Git checkout\n' >&2; exit 1; }

fail()
{
    printf 'gaudere approved production v4: %s\n' "$*" >&2
    exit 1
}

[ "$#" -eq 1 ] \
    || fail "usage: $0 --execute-after-explicit-production-go"
[ "$1" = "--execute-after-explicit-production-go" ] \
    || fail "explicit production authorization argument is required"

command -v git >/dev/null 2>&1 || fail "git is required"
command -v sh >/dev/null 2>&1 || fail "sh is required"

# Exact runtime candidate previously proved on the real Fedora host. These values
# are not secrets. The underlying transaction independently re-resolves both tags
# and requires their immutable IDs to match before stopping production.
candidate_image='localhost/gaudere-agent:schema-v4-proof-20260822T213547Z'
candidate_id='sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3'
rollback_image='localhost/gaudere-agent:rollback-pre-v4-20260822T213547Z'
rollback_id='sha256:6f2dab2ece7783556647f99204e4620a53b6574319310f5c61ffad8b579773d1'
expected_agent_ref='ae094cefee86a3f6c5d0d4d3f868325f378c9376'
expected_core_ref='c24c40b84a12e51515cee4611e3dc79e9fd83892'
representative_task='production-initiative-first'

state_directory="${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"
backup_directory="${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/backups"
service_name='gaudere-agent.service'
canonical_transition="$repo_root/scripts/transition-production-schema-v4-wake-off.sh"
test_mode=${GAUDERE_TEST_MODE:-0}
transition_script=${GAUDERE_APPROVED_TRANSITION_SCRIPT:-$canonical_transition}

case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac
if [ "$test_mode" = "0" ] && [ "$transition_script" != "$canonical_transition" ]; then
    fail "transaction-script override is restricted to synthetic test mode"
fi

# Freeze every repository file that participates directly in the production
# transition. A future edit therefore invalidates this wrapper before any service
# stop or state mutation, even if the edit is otherwise present on a newer main.
verify_blob()
{
    relative=$1
    expected=$2
    path="$repo_root/$relative"
    [ -f "$path" ] || fail "approved helper is missing: $relative"
    actual=$(git -C "$repo_root" hash-object -- "$relative" 2>/dev/null) \
        || fail "cannot hash approved helper: $relative"
    [ "$actual" = "$expected" ] \
        || fail "approved helper drift: $relative expected=$expected found=$actual"
}

verify_blob 'scripts/transition-production-schema-v4-wake-off.sh' \
    '5c75e9aee338968af7cc0324e6a1897e03ff7222'
verify_blob 'scripts/deploy-schema-v4.sh' \
    '9596e64bb45177da5ad26218ba31a526bb90e7ba'
verify_blob 'scripts/deploy-schema-v4-stage-internal.sh' \
    '8435184a3f613bff697572837aad4895248900d3'
verify_blob 'scripts/validate-schema-v4-image-provenance.sh' \
    '85aca98456bfa06142183c3022af021b26e20297'
verify_blob 'scripts/validate-schema-v4-service-wake-off.sh' \
    '1462a773dde12336355aaab61f337c2c765e9411'
verify_blob 'scripts/install-openai-user-service.sh' \
    'c56367230f099a9840c110eedac79760bd6cba91'
verify_blob 'scripts/control-service.sh' \
    'dc6b2c57a2ab06b3191bb1617a96e573d35398b7'
verify_blob 'scripts/backup-state.sh' \
    '01cc2f93dead007391ab463a9ae04b0c57b4c1d4'
verify_blob 'deploy/quadlet/gaudere-agent-openai.container.in' \
    '3321eb30302888a58c81cebbcfc2217cdd4125a4'

[ -f "$transition_script" ] || fail "transaction script not found: $transition_script"

printf 'status=AUTHORIZED_WRAPPER_PREFLIGHT\n'
printf 'candidate_image=%s\n' "$candidate_image"
printf 'candidate_image_id=%s\n' "$candidate_id"
printf 'rollback_image=%s\n' "$rollback_image"
printf 'rollback_image_id=%s\n' "$rollback_id"
printf 'expected_agent_ref=%s\n' "$expected_agent_ref"
printf 'expected_core_ref=%s\n' "$expected_core_ref"
printf 'representative_task=%s\n' "$representative_task"
printf 'approved_helper_blobs=PASS\n'

GAUDERE_STATE_DIR="$state_directory" \
GAUDERE_BACKUP_DIR="$backup_directory" \
GAUDERE_SERVICE_NAME="$service_name" \
GAUDERE_CANDIDATE_IMAGE="$candidate_image" \
GAUDERE_EXPECTED_AGENT_REF="$expected_agent_ref" \
GAUDERE_EXPECTED_CORE_REF="$expected_core_ref" \
GAUDERE_EXPECTED_CANDIDATE_ID="$candidate_id" \
GAUDERE_ROLLBACK_IMAGE="$rollback_image" \
GAUDERE_EXPECTED_ROLLBACK_ID="$rollback_id" \
GAUDERE_SCHEMA_V4_PRODUCTION_AUTHORIZATION=AUTHORIZED_PRODUCTION_SCHEMA_V4_WAKE_OFF \
GAUDERE_TEST_MODE="$test_mode" \
    sh "$transition_script" "$representative_task"
