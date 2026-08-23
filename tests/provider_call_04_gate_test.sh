#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
gate="$repo_root/scripts/run-provider-call-04-gate-v0.sh"
workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-provider04-test.XXXXXX")
cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

fail()
{
    printf 'provider_call_04_gate_test: %s\n' "$*" >&2
    exit 1
}

fake_systemctl="$workspace/systemctl"
cat > "$fake_systemctl" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = "--user" ]
[ "$2" = "is-active" ]
printf 'active\n'
EOF
chmod +x "$fake_systemctl"

fake_podman="$workspace/podman"
cat > "$fake_podman" <<'EOF'
#!/bin/sh
set -eu
runtime=ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
agent=4e6cb09467456f38377bd8610e1ac534c7705380
core=1316cf68db93e4c91a7bd79fbd289b8f382f8659
case "$1:$2" in
    container:inspect)
        printf '%s\n' "$runtime"
        ;;
    image:inspect)
        case "$4" in
            *agent.revision*) printf '%s\n' "$agent" ;;
            *core.revision*) printf '%s\n' "$core" ;;
            *) exit 9 ;;
        esac
        ;;
    *) exit 9 ;;
esac
EOF
chmod +x "$fake_podman"

state="$workspace/state"
mkdir -p "$state"
printf '3\n' > "$state/budget"
printf '0\n' > "$state/task_exists"
printf 'stop\n' > "$state/decision"
: > "$state/log"

fake_control="$workspace/control.sh"
cat > "$fake_control" <<'EOF'
#!/bin/sh
set -eu
state=${FAKE_STATE:?}
command=$1
shift
printf '%s\n' "$command" >> "$state/log"
case "$command" in
    wake-status)
        printf 'gaudere-agent: explicit wake capability is not enabled in this service\n' >&2
        exit 4
        ;;
    budget)
        used=$(cat "$state/budget")
        remaining=$((12 - used))
        printf 'scope="provider.call:openai.responses"\n'
        printf 'provider_enabled=true\n'
        printf 'max_total=12\n'
        printf 'total_used=%s\n' "$used"
        printf 'remaining_total=%s\n' "$remaining"
        printf 'max_window=4\nwindow_seconds=86400\nin_window_used=0\nremaining_window=4\nmin_interval_seconds=900\nlast_consumed_at_ms=123\nnext_new_call=available\n'
        ;;
    task)
        [ "$1" = "production-reflection-wake-source-first" ] || exit 9
        [ "$(cat "$state/task_exists")" = "1" ] || {
            printf 'gaudere-agent: task not found\n' >&2
            exit 3
        }
        decision=$(cat "$state/decision")
        printf 'id="production-reflection-wake-source-first"\n'
        printf 'kind="cognition.reflect.v1"\n'
        printf 'status=succeeded\n'
        printf 'attempts=1/2\n'
        printf 'result_content_type="application/vnd.gaudere.cognition-decision+json"\n'
        if [ "$decision" = "propose" ]; then
            printf 'result_output="{\\"schema\\":\\"gaudere.cognition.decision.v1\\",\\"decision\\":\\"propose_wake\\",\\"reason\\":\\"Resume a self-chosen continuity thread.\\",\\"wake_after_seconds\\":3600}"\n'
        else
            printf 'result_output="{\\"schema\\":\\"gaudere.cognition.decision.v1\\",\\"decision\\":\\"stop\\",\\"reason\\":\\"No self-generated intention requires a delayed revisit.\\"}"\n'
        fi
        printf 'result_metadata_content_type="application/vnd.gaudere.provider-usage+json"\n'
        printf 'result_metadata="{\\"schema\\":\\"gaudere.provider_usage.v1\\"}"\n'
        ;;
    reflect)
        [ "$1" = "production-reflection-wake-source-first" ] || exit 9
        [ "$#" -eq 2 ] || exit 9
        printf '%s\n' "$2" > "$state/objective"
        [ "$(cat "$state/task_exists")" = "0" ] || exit 8
        printf '1\n' > "$state/task_exists"
        printf '4\n' > "$state/budget"
        exec "$0" task production-reflection-wake-source-first
        ;;
    *) exit 9 ;;
esac
EOF
chmod +x "$fake_control"

run_gate()
{
    FAKE_STATE="$state" \
    GAUDERE_TEST_MODE=1 \
    GAUDERE_PROVIDER04_CONTROL_SCRIPT="$fake_control" \
    PODMAN="$fake_podman" SYSTEMCTL="$fake_systemctl" \
        sh "$gate" --execute-after-explicit-provider-call-04-go
}

if FAKE_STATE="$state" GAUDERE_TEST_MODE=1 \
    GAUDERE_PROVIDER04_CONTROL_SCRIPT="$fake_control" \
    PODMAN="$fake_podman" SYSTEMCTL="$fake_systemctl" \
        sh "$gate" >"$workspace/noauth.out" 2>&1; then
    fail "gate accepted execution without explicit authorization argument"
fi

printf '2\n' > "$state/budget"
printf '0\n' > "$state/task_exists"
: > "$state/log"
if run_gate >"$workspace/budget.out" 2>&1; then
    fail "gate accepted a provider budget other than total_used=3"
fi
! grep -qx 'reflect' "$state/log" || fail "budget rejection submitted provider work"

printf '3\n' > "$state/budget"
printf '1\n' > "$state/task_exists"
: > "$state/log"
if run_gate >"$workspace/existing.out" 2>&1; then
    fail "gate accepted a pre-existing source Task ID"
fi
! grep -qx 'reflect' "$state/log" || fail "existing-task rejection submitted provider work"

printf '3\n' > "$state/budget"
printf '0\n' > "$state/task_exists"
printf 'stop\n' > "$state/decision"
: > "$state/log"
run_gate > "$workspace/stop.out"
grep -qx 'decision=stop' "$workspace/stop.out" || fail "stop decision was not preserved"
grep -qx 'canonical_decision=PASS' "$workspace/stop.out" || fail "stop decision was not validated"
grep -qx 'provider_effects=1' "$workspace/stop.out" || fail "provider effect count missing"
grep -qx 'wake_effects=0' "$workspace/stop.out" || fail "wake effect invariant missing"
grep -qx 'gaudere provider call 04 gate: PASS' "$workspace/stop.out" || fail "stop path did not PASS"
[ "$(grep -c '^reflect$' "$state/log")" -eq 1 ] || fail "stop path did not submit exactly once"
[ "$(cat "$state/budget")" = "4" ] || fail "stop path did not consume exactly one synthetic permit"
grep -q 'The experiment is valid either way' "$state/objective" || fail "objective does not make stop explicitly valid"
grep -q 'only if you identify a self-generated thread' "$state/objective" || fail "objective does not guard against coerced wake"

printf '3\n' > "$state/budget"
printf '0\n' > "$state/task_exists"
printf 'propose\n' > "$state/decision"
: > "$state/log"
run_gate > "$workspace/propose.out"
grep -qx 'decision=propose_wake' "$workspace/propose.out" || fail "propose_wake decision was not preserved"
grep -qx 'wake_after_seconds=3600' "$workspace/propose.out" || fail "wake delay was not preserved"
grep -qx 'canonical_decision=PASS' "$workspace/propose.out" || fail "propose_wake decision was not validated"
grep -qx 'wake_effects=0' "$workspace/propose.out" || fail "propose path created a wake effect"
[ "$(grep -c '^reflect$' "$state/log")" -eq 1 ] || fail "propose path did not submit exactly once"

printf 'provider_call_04_gate_test: PASS\n'
