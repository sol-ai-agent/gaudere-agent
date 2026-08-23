#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wrapper="$repo_root/scripts/run-approved-production-schema-v4.sh"
transaction="$repo_root/scripts/transition-production-schema-v4-wake-off.sh"
control="$repo_root/scripts/control-service.sh"
workspace=$(mktemp -d)
transaction_original="$workspace/transaction.original"
control_current="$workspace/control.current"
capture="$workspace/capture"
fake="$workspace/fake-transition.sh"
cp "$transaction" "$transaction_original"
cp "$control" "$control_current"
cleanup()
{
    cp "$transaction_original" "$transaction" 2>/dev/null || true
    cp "$control_current" "$control" 2>/dev/null || true
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

install_approved_control()
{
    cat > "$control" <<'SH'
#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
container=${GAUDERE_CONTAINER:-gaudere-agent}
socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 echo ID TEXT | openai ID TEXT | reflect ID OBJECTIVE | task ID | budget | accept-wake SOURCE_TASK_ID | revoke-wake WAKE_ID REASON | wake WAKE_ID" >&2
    exit 2
fi

exec "$podman_command" exec "$container" \
    /usr/local/bin/gaudere-control --socket "$socket" "$@"
SH
}

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

# Reconstruct the one historical helper that has legitimately evolved after the
# completed production transaction. This proves the frozen wrapper still accepts
# exactly its approved byte set without changing the wrapper or its hashes.
install_approved_control
[ "$(git -C "$repo_root" hash-object -- scripts/control-service.sh)" \
    = 'dc6b2c57a2ab06b3191bb1617a96e573d35398b7' ]

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

# The modern development helper must remain rejected by the historical wrapper.
# This is expected drift after production completed, not a reason to rewrite the
# approved production hashes.
cp "$control_current" "$control"
rm -f "$capture"
if XDG_DATA_HOME="$workspace/data" \
        GAUDERE_TEST_MODE=1 \
        GAUDERE_APPROVED_TRANSITION_SCRIPT="$fake" \
        GAUDERE_TEST_CAPTURE="$capture" \
        sh "$wrapper" --execute-after-explicit-production-go \
        > "$workspace/current-drift.out" 2> "$workspace/current-drift.err"; then
    printf 'wrapper unexpectedly accepted current control-service drift\n' >&2
    exit 1
fi
test ! -e "$capture"
grep -q 'approved helper drift: scripts/control-service.sh' \
    "$workspace/current-drift.err"

# Any drift in another frozen critical helper must still fail before invoking the
# transition. Restore the approved control helper first so this test reaches the
# intended transition hash fence rather than the known current control drift.
install_approved_control
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
cp "$transaction_original" "$transaction"
cp "$control_current" "$control"

printf 'gaudere approved production schema v4 wrapper test: PASS\n'
