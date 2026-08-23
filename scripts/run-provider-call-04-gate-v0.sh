#!/bin/sh
set -eu

# PREP ONLY until Bertrand gives a separate explicit GO for provider call #4.
# This gate submits exactly one bounded reflection and never enables, accepts,
# fires, or revokes a WakeIntent.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
canonical_control="$script_directory/control-service.sh"
control_script=${GAUDERE_PROVIDER04_CONTROL_SCRIPT:-$canonical_control}
podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
container_name=${GAUDERE_CONTAINER:-gaudere-agent}
test_mode=${GAUDERE_TEST_MODE:-0}
max_wait_seconds=${GAUDERE_PROVIDER04_MAX_WAIT_SECONDS:-75}
poll_seconds=${GAUDERE_PROVIDER04_POLL_SECONDS:-1}
task_id=production-reflection-wake-source-first
frozen_runtime_image_id=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
frozen_agent_ref=4e6cb09467456f38377bd8610e1ac534c7705380
frozen_core_ref=1316cf68db93e4c91a7bd79fbd289b8f382f8659
expected_runtime_image_id=${GAUDERE_EXPECTED_RUNTIME_IMAGE_ID:-$frozen_runtime_image_id}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-$frozen_agent_ref}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-$frozen_core_ref}
objective="Context: a previous durable Gaudere initiative, production-initiative-first, chose to make cooperation reliable before ambitious, keep a brief journal of its own proposals and decisions, and derive next priorities from the real project state. Since then production is schema v4, the observable pre-wake runtime is active, provider budget is 3/12 consumed, and WakeIntent is still disabled. Decide whether you now have one concrete intention of your own that is worth carrying across a real delay and asking a later Gaudere instance to resume. The experiment is valid either way: choose stop if no such intention exists. Choose propose_wake only if you identify a self-generated thread you genuinely want to resume; choose the delay from that reason, not to satisfy the test. Bertrand's support and the existence of the experiment are context, not reasons by themselves."

fail()
{
    printf 'gaudere provider call 04 gate: %s\n' "$*" >&2
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

[ "$#" -eq 1 ] || fail "usage: $0 --execute-after-explicit-provider-call-04-go"
[ "$1" = "--execute-after-explicit-provider-call-04-go" ] \
    || fail "explicit provider-call-04 authorization argument is required"

case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac
case "$max_wait_seconds" in
    ''|*[!0-9]*) fail "GAUDERE_PROVIDER04_MAX_WAIT_SECONDS must be a non-negative integer" ;;
esac
case "$poll_seconds" in
    ''|*[!0-9]*) fail "GAUDERE_PROVIDER04_POLL_SECONDS must be a positive integer" ;;
esac
[ "$max_wait_seconds" -gt 0 ] || fail "provider-call-04 wait timeout must be positive"
[ "$poll_seconds" -gt 0 ] || fail "provider-call-04 poll interval must be positive"
if [ "$test_mode" = "0" ]; then
    [ "$control_script" = "$canonical_control" ] \
        || fail "control-script override is restricted to synthetic test mode"
    [ "$expected_runtime_image_id" = "$frozen_runtime_image_id" ] \
        || fail "runtime image override is restricted to synthetic test mode"
    [ "$expected_agent_ref" = "$frozen_agent_ref" ] \
        || fail "Agent ref override is restricted to synthetic test mode"
    [ "$expected_core_ref" = "$frozen_core_ref" ] \
        || fail "Core ref override is restricted to synthetic test mode"
fi

for command in sed tail grep cat mktemp rm sleep python3; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
[ -f "$control_script" ] || fail "control helper not found: $control_script"

expected_runtime_image_id=$(normalize_image_id "$expected_runtime_image_id") \
    || fail "expected runtime image ID is invalid"

service_state=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name must be active"
raw_runtime_image_id=$("$podman_command" container inspect --format '{{.Image}}' "$container_name" 2>/dev/null) \
    || fail "cannot resolve running production image"
runtime_image_id=$(normalize_image_id "$raw_runtime_image_id") \
    || fail "running production image is not one immutable sha256 ID"
[ "$runtime_image_id" = "$expected_runtime_image_id" ] \
    || fail "running image is not the frozen provider-call-04 baseline"
agent_ref=$("$podman_command" image inspect --format '{{index .Labels "io.gaudere.agent.revision"}}' "$runtime_image_id" 2>/dev/null) \
    || fail "cannot inspect Agent provenance label"
core_ref=$("$podman_command" image inspect --format '{{index .Labels "io.gaudere.core.revision"}}' "$runtime_image_id" 2>/dev/null) \
    || fail "cannot inspect Core provenance label"
[ "$agent_ref" = "$expected_agent_ref" ] || fail "running Agent provenance differs from frozen baseline"
[ "$core_ref" = "$expected_core_ref" ] || fail "running Core provenance differs from frozen baseline"

if wake_before=$(sh "$control_script" wake-status 2>&1); then
    printf '%s\n' "$wake_before" >&2
    fail "WakeIntent capability unexpectedly active before provider call #4"
fi
printf '%s\n' "$wake_before"
printf '%s\n' "$wake_before" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "wake-status did not prove WakeIntent disabled"

before_budget=$(sh "$control_script" budget)
printf '%s\n' "$before_budget"
[ "$(report_value provider_enabled "$before_budget")" = "true" ] \
    || fail "provider capability is not enabled"
[ "$(report_value total_used "$before_budget")" = "3" ] \
    || fail "provider call #4 requires exactly three prior durable consumptions"
[ "$(report_value remaining_total "$before_budget")" = "9" ] \
    || fail "provider remaining_total is not 9 before call #4"
[ "$(report_value next_new_call "$before_budget")" = "available" ] \
    || fail "new provider call is not currently admissible"

if existing_task=$(sh "$control_script" task "$task_id" 2>&1); then
    printf '%s\n' "$existing_task" >&2
    fail "source Task ID already exists; refusing a second submission"
fi
printf '%s\n' "$existing_task" | grep -qx 'gaudere-agent: task not found' \
    || fail "source Task preflight was ambiguous; refusing submission"

printf 'status=AUTHORIZED_PROVIDER_CALL_04_PREFLIGHT\n'
printf 'task_id=%s\n' "$task_id"
printf 'runtime_image_id=%s\n' "$runtime_image_id"
printf 'agent_ref=%s\n' "$agent_ref"
printf 'core_ref=%s\n' "$core_ref"
printf 'provider_total_before=3\n'
printf 'wake_capability_before=false\n'

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-provider04.XXXXXX")
cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM
report="$workspace/task.report"

if submission=$(sh "$control_script" reflect "$task_id" "$objective" 2>&1); then
    printf '%s\n' "$submission" > "$report"
else
    printf '%s\n' "$submission" >&2
    # A live-control failure may occur after durable submission. Reconcile by
    # inspection only; never issue reflect a second time.
    if ! sh "$control_script" task "$task_id" > "$report" 2>/dev/null; then
        fail "reflection submission failed and Task cannot be reconciled; do not retry automatically"
    fi
fi

elapsed=0
while :; do
    status=$(sed -n 's/^status=//p' "$report" | tail -n 1)
    case "$status" in
        succeeded) break ;;
        failed|cancelled|manual_review)
            cat "$report" >&2
            fail "reflection reached terminal status $status; provider permit may already be consumed; do not resubmit"
            ;;
        pending|running|cancel_requested|'') ;;
        *) cat "$report" >&2; fail "unexpected Task status: $status" ;;
    esac
    [ "$elapsed" -lt "$max_wait_seconds" ] \
        || { cat "$report" >&2; fail "reflection did not become terminal in time; inspect this Task, do not resubmit"; }
    sleep "$poll_seconds"
    elapsed=$((elapsed + poll_seconds))
    sh "$control_script" task "$task_id" > "$report"
done

cat "$report"
python3 - "$report" > "$workspace/decision" <<'PY'
import json
import sys

fields = {}
with open(sys.argv[1], encoding="utf-8") as source:
    for raw in source:
        raw = raw.rstrip("\n")
        if "=" in raw:
            key, value = raw.split("=", 1)
            fields[key] = value

if fields.get("kind") != '"cognition.reflect.v1"':
    raise SystemExit("unexpected reflection Task kind")
if fields.get("status") != "succeeded":
    raise SystemExit("reflection Task is not succeeded")
if fields.get("result_content_type") != '"application/vnd.gaudere.cognition-decision+json"':
    raise SystemExit("reflection result is not a canonical cognition decision")
if fields.get("result_metadata_content_type") != '"application/vnd.gaudere.provider-usage+json"':
    raise SystemExit("reflection result lacks normalized provider usage metadata")

try:
    decision_text = json.loads(fields["result_output"])
    decision = json.loads(decision_text)
except Exception as exc:
    raise SystemExit(f"cannot parse normalized cognition decision: {exc}")

if decision.get("schema") != "gaudere.cognition.decision.v1":
    raise SystemExit("unexpected cognition decision schema")
action = decision.get("decision")
reason = decision.get("reason")
if action not in {"stop", "propose_wake"} or not isinstance(reason, str) or not reason:
    raise SystemExit("invalid normalized cognition decision")
if action == "stop":
    if set(decision) != {"schema", "decision", "reason"}:
        raise SystemExit("stop decision has unexpected fields")
    print("decision=stop")
else:
    if set(decision) != {"schema", "decision", "reason", "wake_after_seconds"}:
        raise SystemExit("propose_wake decision has unexpected fields")
    delay = decision.get("wake_after_seconds")
    if isinstance(delay, bool) or not isinstance(delay, int) or not 900 <= delay <= 86400:
        raise SystemExit("propose_wake delay is outside the accepted bound")
    print("decision=propose_wake")
    print(f"wake_after_seconds={delay}")
print("canonical_decision=PASS")
PY
cat "$workspace/decision"

after_budget=$(sh "$control_script" budget)
printf '%s\n' "$after_budget"
[ "$(report_value provider_enabled "$after_budget")" = "true" ] \
    || fail "provider unexpectedly became disabled"
[ "$(report_value total_used "$after_budget")" = "4" ] \
    || fail "expected exactly one new durable provider consumption"
[ "$(report_value remaining_total "$after_budget")" = "8" ] \
    || fail "expected remaining_total=8 after provider call #4"

if wake_after=$(sh "$control_script" wake-status 2>&1); then
    printf '%s\n' "$wake_after" >&2
    fail "WakeIntent capability unexpectedly active after provider call #4"
fi
printf '%s\n' "$wake_after" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "WakeIntent disabled state changed after provider call #4"
[ "$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)" = "active" ] \
    || fail "$service_name is not active after provider call #4"

printf 'provider_effects=1\n'
printf 'wake_effects=0\n'
printf 'wake_capability_active=false\n'
printf 'service_final=active\n'
printf 'task_id=%s\n' "$task_id"
printf 'provider_total_after=4\n'
printf 'gaudere provider call 04 gate: PASS\n'
