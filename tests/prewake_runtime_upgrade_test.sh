#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
wrapper="$repository_root/scripts/run-prewake-runtime-upgrade-v0.sh"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-prewake-upgrade.XXXXXX")
state="$workspace/state"
service_state="$workspace/service.state"
gate_log="$workspace/gate.log"
build_log="$workspace/build.log"
gate_done="$workspace/gate.done"
mkdir -p "$state"
printf 'synthetic schema-v4 bytes\n' > "$state/state.db"
printf 'active\n' > "$service_state"
: > "$gate_log"
: > "$build_log"
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

fake_systemctl="$workspace/systemctl"
cat > "$fake_systemctl" <<'SH'
#!/bin/sh
set -eu
[ "$1" = "--user" ] || exit 90
case "$2" in
    is-active)
        cat "$GAUDERE_TEST_SERVICE_STATE_FILE"
        ;;
    stop)
        printf 'inactive\n' > "$GAUDERE_TEST_SERVICE_STATE_FILE"
        ;;
    start)
        printf 'active\n' > "$GAUDERE_TEST_SERVICE_STATE_FILE"
        ;;
    *) exit 91 ;;
esac
SH
chmod +x "$fake_systemctl"

fake_podman="$workspace/podman"
cat > "$fake_podman" <<'SH'
#!/bin/sh
set -eu
case "$1 $2" in
    'container inspect')
        printf 'sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n'
        ;;
    'image inspect')
        printf 'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n'
        ;;
    'image exists')
        if [ "${GAUDERE_TEST_DROP_ROLLBACK:-0}" = "1" ] \
            && [ -f "$GAUDERE_TEST_GATE_DONE" ] \
            && [ "$3" = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]; then
            exit 1
        fi
        exit 0
        ;;
    'run --rm')
        if [ "${GAUDERE_TEST_NO_WAKE_STATUS:-0}" = "1" ]; then
            printf 'Usage: gaudere-control budget | task ID | wake ID\n' >&2
        else
            printf 'Usage: gaudere-control budget | task ID | wake ID | wake-status\n' >&2
        fi
        exit 2
        ;;
    *)
        printf 'unexpected fake podman invocation: %s\n' "$*" >&2
        exit 92
        ;;
esac
SH
chmod +x "$fake_podman"

fake_build="$workspace/build"
cat > "$fake_build" <<'SH'
#!/bin/sh
set -eu
[ -n "$GAUDERE_IMAGE_TAG" ]
printf '%s\n' "$GAUDERE_IMAGE_TAG" >> "$GAUDERE_TEST_BUILD_LOG"
SH
chmod +x "$fake_build"

fake_gate="$workspace/gate"
cat > "$fake_gate" <<'SH'
#!/bin/sh
set -eu
[ "$#" -eq 1 ]
[ "$1" = "production-initiative-first" ]
[ "$GAUDERE_SCHEMA_V4_WAKE_OFF_AUTHORIZATION" = "AUTHORIZED_SCHEMA_V4_WAKE_OFF_GATE" ]
[ "$GAUDERE_EXPECTED_RUNTIME_IMAGE_ID" = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ]
printf 'called\n' >> "$GAUDERE_TEST_GATE_LOG"
printf 'active\n' > "$GAUDERE_TEST_SERVICE_STATE_FILE"
: > "$GAUDERE_TEST_GATE_DONE"
printf 'gaudere schema v4 wake-off service gate: PASS\n'
printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'runtime_image_identity=PASS\n'
SH
chmod +x "$fake_gate"

fake_control="$workspace/control"
cat > "$fake_control" <<'SH'
#!/bin/sh
set -eu
case "$1" in
    budget)
        printf 'provider_enabled=true\n'
        printf 'total_used=%s\n' "${GAUDERE_TEST_PROVIDER_TOTAL:-3}"
        ;;
    task)
        [ "$2" = "production-initiative-first" ]
        printf 'status=succeeded\n'
        printf 'result_metadata_content_type="application/vnd.gaudere.provider-usage+json"\n'
        ;;
    wake-status)
        printf 'gaudere-agent: explicit wake capability is not enabled in this service\n' >&2
        exit 4
        ;;
    *) exit 93 ;;
esac
SH
chmod +x "$fake_control"

run_wrapper()
{
    GAUDERE_TEST_MODE=1 \
    GAUDERE_STATE_DIR="$state" \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    GAUDERE_EXPECTED_PREVIOUS_IMAGE_ID=sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb \
    GAUDERE_TEST_SERVICE_STATE_FILE="$service_state" \
    GAUDERE_TEST_GATE_LOG="$gate_log" \
    GAUDERE_TEST_BUILD_LOG="$build_log" \
    GAUDERE_TEST_GATE_DONE="$gate_done" \
    GAUDERE_PREWAKE_BUILD_SCRIPT="$fake_build" \
    GAUDERE_PREWAKE_WAKE_OFF_GATE="$fake_gate" \
    GAUDERE_PREWAKE_CONTROL_SCRIPT="$fake_control" \
    PODMAN="$fake_podman" SYSTEMCTL="$fake_systemctl" \
        sh "$wrapper" --execute-after-explicit-production-go
}

if sh "$wrapper" > "$workspace/noauth.out" 2> "$workspace/noauth.err"; then
    printf 'prewake wrapper ran without explicit authorization argument\n' >&2
    exit 1
fi
grep -q 'explicit production authorization argument is required\|usage:' "$workspace/noauth.err"

run_wrapper > "$workspace/success.out"
grep -q '^gaudere prewake runtime upgrade: PASS$' "$workspace/success.out"
grep -q '^provider_effects=0$' "$workspace/success.out"
grep -q '^wake_effects=0$' "$workspace/success.out"
grep -q '^wake_capability_active=false$' "$workspace/success.out"
grep -q '^wake_status_surface=present$' "$workspace/success.out"
grep -q '^candidate_image_id=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa$' \
    "$workspace/success.out"
grep -q '^rollback_image_id=sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb$' \
    "$workspace/success.out"
grep -q '^rollback_image_retained=true$' "$workspace/success.out"
[ "$(cat "$service_state")" = "active" ]
[ "$(wc -l < "$build_log" | tr -d ' ')" -eq 1 ]
[ "$(wc -l < "$gate_log" | tr -d ' ')" -eq 1 ]

# Missing wake-status must fail before the production service is stopped and before
# the wake-off gate is entered.
printf 'active\n' > "$service_state"
rm -f "$gate_done"
: > "$gate_log"
GAUDERE_TEST_NO_WAKE_STATUS=1 run_wrapper \
    > "$workspace/no-status.out" 2> "$workspace/no-status.err" && {
        printf 'prewake wrapper accepted a candidate without wake-status\n' >&2
        exit 1
    }
grep -q 'candidate image does not expose wake-status observability' "$workspace/no-status.err"
[ "$(cat "$service_state")" = "active" ]
[ ! -s "$gate_log" ]

# The upgrade belongs before provider permit #4. A changed durable count fails
# before image build or service mutation.
printf 'active\n' > "$service_state"
rm -f "$gate_done"
: > "$gate_log"
GAUDERE_TEST_PROVIDER_TOTAL=4 run_wrapper \
    > "$workspace/budget.out" 2> "$workspace/budget.err" && {
        printf 'prewake wrapper accepted provider total other than three\n' >&2
        exit 1
    }
grep -q 'requires exactly three durable provider consumptions' "$workspace/budget.err"
[ "$(cat "$service_state")" = "active" ]
[ ! -s "$gate_log" ]

# Losing the old immutable image after a nominal candidate gate must not be reported
# as success. The service may be active, but rollback integrity is a hard result.
printf 'active\n' > "$service_state"
rm -f "$gate_done"
: > "$gate_log"
GAUDERE_TEST_DROP_ROLLBACK=1 run_wrapper \
    > "$workspace/rollback.out" 2> "$workspace/rollback.err" && {
        printf 'prewake wrapper accepted loss of rollback image\n' >&2
        exit 1
    }
grep -q 'frozen rollback image disappeared after successful upgrade' "$workspace/rollback.err"
[ "$(cat "$service_state")" = "active" ]
[ "$(wc -l < "$gate_log" | tr -d ' ')" -eq 1 ]

printf 'prewake runtime upgrade synthetic test: PASS\n'
