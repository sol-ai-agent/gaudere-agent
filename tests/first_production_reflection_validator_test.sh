#!/bin/sh
set -eu

workspace=$(mktemp -d)
trap 'rm -rf "$workspace"' EXIT HUP INT TERM
state="$workspace/state"
calls="$workspace/calls"
validator="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/scripts/validate-first-production-reflection.sh"
objective='Assess this first permanent bounded-reflection proof. Choose stop or propose one inert future wake only if useful; do not claim authority to act.'

cat > "$workspace/systemctl" <<'SH'
#!/bin/sh
if [ "$1" = "--user" ] && [ "$2" = "is-active" ]; then
    printf 'active\n'
    exit 0
fi
exit 2
SH
chmod +x "$workspace/systemctl"

cat > "$workspace/control.sh" <<'SH'
#!/bin/sh
set -eu
phase=$(cat "$FAKE_STATE")
case "$1" in
    budget)
        if [ "$phase" = "0" ]; then
            cat <<'EOF'
scope="provider.call:openai.responses"
provider_enabled=true
max_total=12
total_used=1
remaining_total=11
max_window=4
window_seconds=86400
in_window_used=1
remaining_window=3
min_interval_seconds=900
last_consumed_at_ms=1787341928220
next_new_call=available
EOF
        else
            cat <<'EOF'
scope="provider.call:openai.responses"
provider_enabled=true
max_total=12
total_used=2
remaining_total=10
max_window=4
window_seconds=86400
in_window_used=2
remaining_window=2
min_interval_seconds=900
last_consumed_at_ms=1787342928220
next_new_call=cooldown
EOF
        fi
        ;;
    task)
        [ "$2" = "production-reflection-first" ] || exit 90
        if [ "$phase" = "0" ]; then
            printf 'gaudere-agent: task not found\n' >&2
            exit 3
        fi
        case "$FAKE_DECISION" in
            propose_wake)
                result='{"decision":"propose_wake","reason":"A later bounded review may be useful.","schema":"gaudere.cognition.decision.v1","wake_after_seconds":900}'
                ;;
            stop)
                result='{"decision":"stop","reason":"The capability proof is complete.","schema":"gaudere.cognition.decision.v1"}'
                ;;
            invalid)
                result='{"decision":"propose_wake","reason":"Too soon.","schema":"gaudere.cognition.decision.v1","wake_after_seconds":899}'
                ;;
            *) exit 91 ;;
        esac
        encoded_result=$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1], ensure_ascii=False))' "$result")
        cat <<EOF
id="production-reflection-first"
kind="cognition.reflect.v1"
status=succeeded
attempts=1/2
result_content_type="application/vnd.gaudere.cognition-decision+json"
result_output=$encoded_result
result_metadata_content_type="application/vnd.gaudere.provider-usage+json"
result_metadata="{\"cache_write_input_tokens\":0,\"cached_input_tokens\":0,\"input_tokens\":24,\"model\":\"gpt-5.6-sol\",\"output_tokens\":6,\"provider\":\"openai\",\"reasoning_tokens\":2,\"schema\":\"gaudere.provider_usage.v1\",\"total_tokens\":30}"
EOF
        ;;
    reflect)
        [ "$#" -eq 3 ] || exit 92
        [ "$2" = "production-reflection-first" ] || exit 93
        [ "$3" = "$FAKE_OBJECTIVE" ] || exit 94
        [ "$phase" = "0" ] || exit 95
        count=$(cat "$FAKE_CALLS")
        count=$((count + 1))
        printf '%s\n' "$count" > "$FAKE_CALLS"
        [ "$count" -eq 1 ] || exit 96
        printf '1\n' > "$FAKE_STATE"
        cat <<'EOF'
id="production-reflection-first"
kind="cognition.reflect.v1"
status=pending
attempts=0/2
EOF
        ;;
    openai)
        exit 97
        ;;
    *)
        exit 2
        ;;
esac
SH
chmod +x "$workspace/control.sh"

reset_fake()
{
    printf '0\n' > "$state"
    printf '0\n' > "$calls"
}

run_validator()
{
    decision=$1
    output=$2
    FAKE_STATE="$state" FAKE_CALLS="$calls" \
    FAKE_DECISION="$decision" FAKE_OBJECTIVE="$objective" \
    SYSTEMCTL="$workspace/systemctl" \
    GAUDERE_CONTROL_SCRIPT="$workspace/control.sh" \
    GAUDERE_VALIDATION_POLL_SECONDS=0 \
    sh "$validator" > "$output" 2>&1
}

reset_fake
run_validator propose_wake "$workspace/propose.out"
grep -q '^task_identity_unused=PASS$' "$workspace/propose.out"
grep -q '^normalized_decision=PASS$' "$workspace/propose.out"
grep -q '^decision=propose_wake$' "$workspace/propose.out"
grep -q '^wake_after_seconds=900$' "$workspace/propose.out"
grep -q '^normalized_usage=PASS$' "$workspace/propose.out"
grep -q '^total_tokens=30$' "$workspace/propose.out"
grep -q '^gaudere first production reflection validation: PASS$' \
    "$workspace/propose.out"
test "$(cat "$calls")" = "1"

# The same durable identity must stop a rerun before any second submission.
if run_validator propose_wake "$workspace/rerun.out"; then
    printf 'validator unexpectedly accepted an existing reflection task\n' >&2
    exit 1
fi
grep -q 'already exists; refusing to submit or replay' "$workspace/rerun.out"
test "$(cat "$calls")" = "1"

reset_fake
run_validator stop "$workspace/stop.out"
grep -q '^decision=stop$' "$workspace/stop.out"
if grep -q '^wake_after_seconds=' "$workspace/stop.out"; then
    printf 'stop decision unexpectedly reported a wake delay\n' >&2
    exit 1
fi
test "$(cat "$calls")" = "1"

# Even if a mocked live owner returned an invalid normalized decision, the
# validator must fail after one call and the durable identity must block replay.
reset_fake
if run_validator invalid "$workspace/invalid.out"; then
    printf 'validator unexpectedly accepted an out-of-range wake proposal\n' >&2
    exit 1
fi
grep -q 'wake proposal delay is outside the bounded contract' \
    "$workspace/invalid.out"
test "$(cat "$calls")" = "1"
if run_validator invalid "$workspace/invalid-rerun.out"; then
    printf 'validator unexpectedly replayed an invalid durable result\n' >&2
    exit 1
fi
grep -q 'already exists; refusing to submit or replay' \
    "$workspace/invalid-rerun.out"
test "$(cat "$calls")" = "1"

reset_fake
if FAKE_STATE="$state" FAKE_CALLS="$calls" \
    FAKE_DECISION=stop FAKE_OBJECTIVE="$objective" \
    SYSTEMCTL="$workspace/systemctl" \
    GAUDERE_CONTROL_SCRIPT="$workspace/control.sh" \
    sh "$validator" unexpected > "$workspace/argument.out" 2>&1; then
    printf 'validator unexpectedly accepted an argument\n' >&2
    exit 1
fi
grep -q 'accepts no arguments' "$workspace/argument.out"
test "$(cat "$calls")" = "0"

printf 'gaudere first production reflection validator offline test: PASS\n'
