#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
wrapper="$repository_root/scripts/deploy-schema-v4.sh"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-schema-v4-provenance.XXXXXX")
state="$workspace/state"
backups="$workspace/backups"
provenance_log="$workspace/provenance.log"
stage_log="$workspace/stage.log"
mkdir -p "$state" "$backups"
trap 'rm -rf "$workspace"' EXIT HUP INT TERM
printf 'synthetic state bytes\n' > "$state/state.db"

fake_systemctl="$workspace/systemctl"
cat > "$fake_systemctl" <<'SH'
#!/bin/sh
[ "$#" -eq 3 ] && [ "$1" = "--user" ] && [ "$2" = "is-active" ] || exit 90
printf '%s\n' "${GAUDERE_TEST_SERVICE_STATE:-inactive}"
SH
chmod +x "$fake_systemctl"

fake_provenance="$workspace/provenance"
cat > "$fake_provenance" <<'SH'
#!/bin/sh
set -eu
[ "$#" -eq 1 ]
[ -f "$1" ] && [ -f "$1.sha256" ]
[ "$GAUDERE_CANDIDATE_IMAGE" = "localhost/gaudere-agent:candidate-v4-test" ]
[ "$GAUDERE_EXPECTED_AGENT_REF" = "1111111111111111111111111111111111111111" ]
[ "$GAUDERE_EXPECTED_CORE_REF" = "2222222222222222222222222222222222222222" ]
[ "$GAUDERE_EXPECTED_CANDIDATE_ID" = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ]
[ "$GAUDERE_ROLLBACK_IMAGE" = "localhost/gaudere-agent:rollback-v4-test" ]
[ "$GAUDERE_EXPECTED_ROLLBACK_ID" = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]
printf 'called\n' >> "$GAUDERE_TEST_PROVENANCE_LOG"
if [ "${GAUDERE_TEST_PROVENANCE_FAIL:-0}" = "1" ]; then
    printf 'synthetic provenance failure\n' >&2
    exit 91
fi
printf 'status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION\n'
printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'production_state_touched=false\n'
printf 'gaudere schema v4 image provenance validation: PASS\n'
SH
chmod +x "$fake_provenance"

fake_stage="$workspace/stage"
cat > "$fake_stage" <<'SH'
#!/bin/sh
set -eu
[ "$#" -eq 0 ]
[ "$GAUDERE_IMAGE" = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ]
printf 'called image=%s\n' "$GAUDERE_IMAGE" >> "$GAUDERE_TEST_STAGE_LOG"
printf 'stage_called=PASS\n'
SH
chmod +x "$fake_stage"

run_wrapper()
{
    GAUDERE_TEST_MODE=1 \
    GAUDERE_STATE_DIR="$state" \
    GAUDERE_BACKUP_DIR="$backups" \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    SYSTEMCTL="$fake_systemctl" \
    GAUDERE_SCHEMA_V4_IMAGE_PROVENANCE_VALIDATOR="$fake_provenance" \
    GAUDERE_SCHEMA_V4_STAGE_SCRIPT="$fake_stage" \
    GAUDERE_CANDIDATE_IMAGE=localhost/gaudere-agent:candidate-v4-test \
    GAUDERE_EXPECTED_AGENT_REF=1111111111111111111111111111111111111111 \
    GAUDERE_EXPECTED_CORE_REF=2222222222222222222222222222222222222222 \
    GAUDERE_EXPECTED_CANDIDATE_ID=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
    GAUDERE_ROLLBACK_IMAGE=localhost/gaudere-agent:rollback-v4-test \
    GAUDERE_EXPECTED_ROLLBACK_ID=sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb \
    GAUDERE_TEST_PROVENANCE_LOG="$provenance_log" \
    GAUDERE_TEST_STAGE_LOG="$stage_log" \
        sh "$wrapper"
}

run_wrapper > "$workspace/success.out"
grep -q '^image_provenance_gate=PASS$' "$workspace/success.out"
grep -q '^stage_called=PASS$' "$workspace/success.out"
grep -q '^candidate_image_id=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa$' \
    "$workspace/success.out"
[ "$(wc -l < "$provenance_log" | tr -d ' ')" -eq 1 ]
[ "$(wc -l < "$stage_log" | tr -d ' ')" -eq 1 ]
grep -q 'image=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa$' \
    "$stage_log"

GAUDERE_TEST_PROVENANCE_FAIL=1 \
    run_wrapper > "$workspace/fail.out" 2> "$workspace/fail.err" && {
        printf 'wrapper accepted failed provenance\n' >&2
        exit 1
    }
grep -q 'image provenance contract failed' "$workspace/fail.err"
[ "$(wc -l < "$stage_log" | tr -d ' ')" -eq 1 ]

if GAUDERE_TEST_MODE=0 \
    GAUDERE_STATE_DIR="$state" \
    GAUDERE_BACKUP_DIR="$backups" \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    SYSTEMCTL="$fake_systemctl" \
    GAUDERE_SCHEMA_V4_IMAGE_PROVENANCE_VALIDATOR="$fake_provenance" \
    GAUDERE_SCHEMA_V4_STAGE_SCRIPT="$fake_stage" \
    GAUDERE_CANDIDATE_IMAGE=localhost/gaudere-agent:candidate-v4-test \
    GAUDERE_EXPECTED_AGENT_REF=1111111111111111111111111111111111111111 \
    GAUDERE_EXPECTED_CORE_REF=2222222222222222222222222222222222222222 \
    GAUDERE_EXPECTED_CANDIDATE_ID=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
    GAUDERE_ROLLBACK_IMAGE=localhost/gaudere-agent:rollback-v4-test \
    GAUDERE_EXPECTED_ROLLBACK_ID=sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb \
        sh "$wrapper" > "$workspace/override.out" 2> "$workspace/override.err"; then
    printf 'production mode accepted synthetic override\n' >&2
    exit 1
fi
grep -q 'override is restricted to synthetic test mode' "$workspace/override.err"

GAUDERE_TEST_SERVICE_STATE=active \
    run_wrapper > "$workspace/active.out" 2> "$workspace/active.err" && {
        printf 'wrapper accepted active service\n' >&2
        exit 1
    }
grep -q 'must report exactly inactive (found active)' "$workspace/active.err"

printf 'gaudere schema v4 provenance integration tests: PASS\n'
