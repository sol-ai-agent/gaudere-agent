#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wrapper="$repo_root/scripts/run-approved-production-schema-v4.sh"
transaction="$repo_root/scripts/transition-production-schema-v4-wake-off.sh"
workspace=$(mktemp -d)
original="$workspace/transaction.original"
capture="$workspace/capture"
fake="$workspace/fake-transition.sh"
cp "$transaction" "$original"
cleanup()
{
    cp "$original" "$transaction" 2>/dev/null || true
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

cat > "$fake" <<'SH'
#!/bin/sh
set -eu
{
    printf 'arg=%s\n' "$1"
    printf 'state=%s\n' "$GAUDERE_STATE_DIR"
    printf 'backups=%s\n' "$GAUDERE_BACKUP_DIR"
    printf 'service=%s\n' "$GAUDERE_SERVICE_NAME"
    printf 'candidate=%s\n' "$GAUDERE_CANDIDATE_IMAGE"
    printf 'candidate_id=%s\n' "$GAUDERE_EXPECTED_CANDIDATE_ID"
    printf 'agent_ref=%s\n' "$GAUDERE_EXPECTED_AGENT_REF"
    printf 'core_ref=%s\n' "$GAUDERE_EXPECTED_CORE_REF"
    printf 'rollback=%s\n' "$GAUDERE_ROLLBACK_IMAGE"
    printf 'rollback_id=%s\n' "$GAUDERE_EXPECTED_ROLLBACK_ID"
    printf 'authorization=%s\n' "$GAUDERE_SCHEMA_V4_PRODUCTION_AUTHORIZATION"
    printf 'test_mode=%s\n' "$GAUDERE_TEST_MODE"
} > "$GAUDERE_TEST_CAPTURE"
SH
chmod +x "$fake"

# No exact human-GO argument means no transition invocation.
if GAUDERE_TEST_MODE=1 GAUDERE_APPROVED_TRANSITION_SCRIPT="$fake" \
        GAUDERE_TEST_CAPTURE="$capture" sh "$wrapper" \
        > "$workspace/no-go.out" 2> "$workspace/no-go.err"; then
    printf 'wrapper unexpectedly ran without explicit GO argument\n' >&2
    exit 1
fi
test ! -e "$capture"
grep -q 'usage: .*--execute-after-explicit-production-go' "$workspace/no-go.err"

# The authorized synthetic path exports exactly the frozen production identities.
XDG_DATA_HOME="$workspace/data" \
GAUDERE_TEST_MODE=1 \
GAUDERE_APPROVED_TRANSITION_SCRIPT="$fake" \
GAUDERE_TEST_CAPTURE="$capture" \
    sh "$wrapper" --execute-after-explicit-production-go \
    > "$workspace/pass.out"

grep -qx 'approved_helper_blobs=PASS' "$workspace/pass.out"
grep -qx 'arg=production-initiative-first' "$capture"
grep -qx "state=$workspace/data/gaudere/state" "$capture"
grep -qx "backups=$workspace/data/gaudere/backups" "$capture"
grep -qx 'service=gaudere-agent.service' "$capture"
grep -qx 'candidate=localhost/gaudere-agent:schema-v4-proof-20260822T213547Z' "$capture"
grep -qx 'candidate_id=sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3' "$capture"
grep -qx 'agent_ref=ae094cefee86a3f6c5d0d4d3f868325f378c9376' "$capture"
grep -qx 'core_ref=c24c40b84a12e51515cee4611e3dc79e9fd83892' "$capture"
grep -qx 'rollback=localhost/gaudere-agent:rollback-pre-v4-20260822T213547Z' "$capture"
grep -qx 'rollback_id=sha256:6f2dab2ece7783556647f99204e4620a53b6574319310f5c61ffad8b579773d1' "$capture"
grep -qx 'authorization=AUTHORIZED_PRODUCTION_SCHEMA_V4_WAKE_OFF' "$capture"
grep -qx 'test_mode=1' "$capture"

# Any drift in one frozen critical helper must fail before invoking the transition.
rm -f "$capture"
printf '\n# synthetic drift\n' >> "$transaction"
if XDG_DATA_HOME="$workspace/data" \
        GAUDERE_TEST_MODE=1 \
        GAUDERE_APPROVED_TRANSITION_SCRIPT="$fake" \
        GAUDERE_TEST_CAPTURE="$capture" \
        sh "$wrapper" --execute-after-explicit-production-go \
        > "$workspace/drift.out" 2> "$workspace/drift.err"; then
    printf 'wrapper unexpectedly accepted critical helper drift\n' >&2
    exit 1
fi
test ! -e "$capture"
grep -q 'approved helper drift: scripts/transition-production-schema-v4-wake-off.sh' \
    "$workspace/drift.err"
cp "$original" "$transaction"

printf 'gaudere approved production schema v4 wrapper test: PASS\n'
