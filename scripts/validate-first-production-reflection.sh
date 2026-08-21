#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script=${GAUDERE_CONTROL_SCRIPT:-"$script_directory/control-service.sh"}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
expected_model=gpt-5.6-sol
task_id=production-reflection-first
objective='Assess this first permanent bounded-reflection proof. Choose stop or propose one inert future wake only if useful; do not claim authority to act.'
max_checks=${GAUDERE_VALIDATION_MAX_CHECKS:-75}
poll_seconds=${GAUDERE_VALIDATION_POLL_SECONDS:-1}

fail()
{
    printf 'gaudere first production reflection validation: %s\n' "$*" >&2
    exit 1
}

say()
{
    printf '\n==> %s\n' "$*"
}

budget_value()
{
    key=$1
    text_value=$2
    printf '%s\n' "$text_value" | sed -n "s/^${key}=//p" | tail -n 1
}

[ "$#" -eq 0 ] \
    || fail "this validator accepts no arguments; task identity and objective are fixed"
[ -x "$control_script" ] || [ -f "$control_script" ] \
    || fail "control script not found: $control_script"
command -v "$systemctl_command" >/dev/null 2>&1 \
    || fail "systemctl command not found: $systemctl_command"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

case "$max_checks" in
    ''|*[!0-9]*) fail "GAUDERE_VALIDATION_MAX_CHECKS must be an integer" ;;
esac
[ "$max_checks" -gt 0 ] || fail "validation check limit must be positive"
case "$poll_seconds" in
    ''|*[!0-9]*) fail "GAUDERE_VALIDATION_POLL_SECONDS must be an integer" ;;
esac

service_state=$($systemctl_command --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name is not active"

say "prove the fixed reflection identity has never been submitted"
if existing=$(sh "$control_script" task "$task_id" 2>&1); then
    printf '%s\n' "$existing" >&2
    fail "durable task $task_id already exists; refusing to submit or replay"
else
    existing_status=$?
fi
[ "$existing_status" -eq 3 ] \
    || { printf '%s\n' "$existing" >&2; fail "cannot prove task identity is unused"; }
printf 'task_identity_unused=PASS\n'

say "prove exactly one lifetime permit is used and one new call is admissible"
before=$(sh "$control_script" budget)
printf '%s\n' "$before"
[ "$(budget_value scope "$before")" = '"provider.call:openai.responses"' ] \
    || fail "unexpected provider budget scope"
[ "$(budget_value provider_enabled "$before")" = "true" ] \
    || fail "provider is not enabled"
[ "$(budget_value max_total "$before")" = "12" ] \
    || fail "unexpected lifetime provider budget"
[ "$(budget_value total_used "$before")" = "1" ] \
    || fail "first-reflection validator requires total_used=1"
[ "$(budget_value remaining_total "$before")" = "11" ] \
    || fail "first-reflection validator requires remaining_total=11"
[ "$(budget_value max_window "$before")" = "4" ] \
    || fail "unexpected rolling-window provider budget"
[ "$(budget_value window_seconds "$before")" = "86400" ] \
    || fail "unexpected rolling-window duration"
[ "$(budget_value min_interval_seconds "$before")" = "900" ] \
    || fail "unexpected provider minimum interval"
[ "$(budget_value next_new_call "$before")" = "available" ] \
    || fail "new provider call is not currently admissible"

before_window=$(budget_value in_window_used "$before")
case "$before_window" in
    0|1) ;;
    *) fail "total_used=1 requires in_window_used to be 0 or 1" ;;
esac
expected_before_remaining=$((4 - before_window))
[ "$(budget_value remaining_window "$before")" = "$expected_before_remaining" ] \
    || fail "rolling-window remainder is inconsistent before reflection"
before_consumed_at=$(budget_value last_consumed_at_ms "$before")
case "$before_consumed_at" in
    ''|none|*[!0-9]*) fail "existing lifetime permit has no valid timestamp" ;;
esac

say "submit exactly one fixed bounded-reflection task"
submission=$(sh "$control_script" reflect "$task_id" "$objective")
printf '%s\n' "$submission"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-first-reflection.XXXXXX")
cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM
report="$workspace/task.report"
printf '%s\n' "$submission" > "$report"

checks=0
while :; do
    status=$(sed -n 's/^status=//p' "$report" | tail -n 1)
    case "$status" in
        succeeded)
            break
            ;;
        failed|cancelled|manual_review)
            cat "$report" >&2
            fail "reflection task reached terminal status $status"
            ;;
        pending|running|cancel_requested|'')
            ;;
        *)
            cat "$report" >&2
            fail "unexpected reflection task status: $status"
            ;;
    esac

    [ "$checks" -lt "$max_checks" ] \
        || { cat "$report" >&2; fail "reflection task did not become terminal in time"; }
    sleep "$poll_seconds"
    checks=$((checks + 1))
    sh "$control_script" task "$task_id" > "$report"
done

say "inspect the normalized durable decision and provider usage"
cat "$report"
python3 - "$report" "$task_id" "$expected_model" <<'PY'
import json
import sys

path, expected_id, expected_model = sys.argv[1:]
fields = {}
with open(path, encoding="utf-8") as source:
    for raw in source:
        raw = raw.rstrip("\n")
        if "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        if key in fields:
            raise SystemExit(f"duplicate task report field: {key}")
        fields[key] = value

def decoded_field(name):
    if name not in fields:
        raise SystemExit(f"missing task report field: {name}")
    try:
        value = json.loads(fields[name])
    except json.JSONDecodeError as error:
        raise SystemExit(f"invalid encoded task report field {name}: {error}")
    if not isinstance(value, str):
        raise SystemExit(f"task report field {name} is not text")
    return value

def strict_object(text, label):
    def reject_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(text, object_pairs_hook=reject_duplicates)
    except (json.JSONDecodeError, ValueError) as error:
        raise SystemExit(f"invalid {label} JSON: {error}")
    if not isinstance(value, dict):
        raise SystemExit(f"{label} is not a JSON object")
    return value

if decoded_field("id") != expected_id:
    raise SystemExit("unexpected reflection task identity")
if decoded_field("kind") != "cognition.reflect.v1":
    raise SystemExit("unexpected reflection task kind")
if fields.get("status") != "succeeded":
    raise SystemExit("reflection task is not succeeded")
if fields.get("attempts") != "1/2":
    raise SystemExit("unexpected reflection task attempt count")
if decoded_field("result_content_type") != "application/vnd.gaudere.cognition-decision+json":
    raise SystemExit("missing normalized cognition decision content type")

decision = strict_object(decoded_field("result_output"), "decision")
if decision.get("schema") != "gaudere.cognition.decision.v1":
    raise SystemExit("unexpected cognition decision schema")
action = decision.get("decision")
reason = decision.get("reason")
if not isinstance(reason, str) or not 1 <= len(reason.encode("utf-8")) <= 1024:
    raise SystemExit("invalid bounded reflection reason")
if action == "stop":
    if set(decision) != {"schema", "decision", "reason"}:
        raise SystemExit("stop decision has unexpected fields")
elif action == "propose_wake":
    if set(decision) != {"schema", "decision", "reason", "wake_after_seconds"}:
        raise SystemExit("wake proposal has unexpected fields")
    delay = decision["wake_after_seconds"]
    if not isinstance(delay, int) or isinstance(delay, bool) or not 900 <= delay <= 86400:
        raise SystemExit("wake proposal delay is outside the bounded contract")
else:
    raise SystemExit(f"unsupported cognition decision: {action!r}")

if decoded_field("result_metadata_content_type") != "application/vnd.gaudere.provider-usage+json":
    raise SystemExit("missing normalized provider usage content type")
metadata = strict_object(decoded_field("result_metadata"), "provider usage")
required_identity = {
    "schema": "gaudere.provider_usage.v1",
    "provider": "openai",
    "model": expected_model,
}
for key, expected in required_identity.items():
    if metadata.get(key) != expected:
        raise SystemExit(f"unexpected usage metadata {key}: {metadata.get(key)!r}")

count_fields = (
    "input_tokens",
    "cached_input_tokens",
    "cache_write_input_tokens",
    "output_tokens",
    "reasoning_tokens",
    "total_tokens",
)
for key in count_fields:
    value = metadata.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise SystemExit(f"invalid usage token count {key}")
if metadata["input_tokens"] <= 0 or metadata["output_tokens"] <= 0:
    raise SystemExit("successful proof must record non-zero input and output tokens")
if metadata["total_tokens"] != metadata["input_tokens"] + metadata["output_tokens"]:
    raise SystemExit("usage total_tokens does not match input + output")
if metadata["cached_input_tokens"] > metadata["input_tokens"]:
    raise SystemExit("cached input tokens exceed total input tokens")
if metadata["reasoning_tokens"] > metadata["output_tokens"]:
    raise SystemExit("reasoning tokens exceed output tokens")

print("normalized_decision=PASS")
print(f"decision={action}")
if action == "propose_wake":
    print(f"wake_after_seconds={decision['wake_after_seconds']}")
print("normalized_usage=PASS")
for key in count_fields:
    print(f"{key}={metadata[key]}")
PY

say "prove exactly one additional lifetime permit was consumed"
after=$(sh "$control_script" budget)
printf '%s\n' "$after"
[ "$(budget_value provider_enabled "$after")" = "true" ] \
    || fail "provider unexpectedly became disabled"
[ "$(budget_value total_used "$after")" = "2" ] \
    || fail "expected total_used=2 after first production reflection"
[ "$(budget_value remaining_total "$after")" = "10" ] \
    || fail "expected remaining_total=10"

expected_after_window=$((before_window + 1))
expected_after_remaining=$((4 - expected_after_window))
[ "$(budget_value in_window_used "$after")" = "$expected_after_window" ] \
    || fail "rolling-window usage did not increase by exactly one"
[ "$(budget_value remaining_window "$after")" = "$expected_after_remaining" ] \
    || fail "rolling-window remainder is inconsistent after reflection"
after_consumed_at=$(budget_value last_consumed_at_ms "$after")
case "$after_consumed_at" in
    ''|none|*[!0-9]*) fail "reflection did not record a valid consumption timestamp" ;;
esac
[ "$after_consumed_at" != "$before_consumed_at" ] \
    || fail "reflection did not advance the durable consumption timestamp"
[ "$(budget_value next_new_call "$after")" = "cooldown" ] \
    || fail "expected cooldown immediately after reflection"

service_state=$($systemctl_command --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name is not active after reflection"

say "first permanent bounded-reflection lifecycle proof complete"
printf 'gaudere first production reflection validation: PASS\n'
