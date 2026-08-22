#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script=${GAUDERE_CONTROL_SCRIPT:-"$script_directory/control-service.sh"}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
expected_model=${GAUDERE_OPENAI_MODEL:-gpt-5.6-sol}
max_wait_seconds=${GAUDERE_VALIDATION_MAX_WAIT_SECONDS:-75}
poll_seconds=${GAUDERE_VALIDATION_POLL_SECONDS:-1}

fail()
{
    printf 'gaudere next production OpenAI validation: %s\n' "$*" >&2
    exit 1
}

usage()
{
    printf 'Usage: %s EXPECTED_TOTAL_USED TASK_ID TEXT\n' "$0" >&2
    exit 2
}

[ "$#" -eq 3 ] || usage
expected_before=$1
task_id=$2
text=$3

case "$expected_before" in
    ''|*[!0-9]*) fail "EXPECTED_TOTAL_USED must be a non-negative integer" ;;
esac
case "$max_wait_seconds" in
    ''|*[!0-9]*) fail "GAUDERE_VALIDATION_MAX_WAIT_SECONDS must be an integer" ;;
esac
[ "$max_wait_seconds" -gt 0 ] || fail "wait timeout must be positive"
[ -n "$task_id" ] || fail "TASK_ID must not be empty"
[ -n "$text" ] || fail "TEXT must not be empty"

[ -x "$control_script" ] || [ -f "$control_script" ] \
    || fail "control script not found: $control_script"
command -v "$systemctl_command" >/dev/null 2>&1 \
    || fail "systemctl command not found: $systemctl_command"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

service_state=$($systemctl_command --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name is not active"

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

numeric_budget_value()
{
    key=$1
    text_value=$2
    value=$(budget_value "$key" "$text_value")
    case "$value" in
        ''|*[!0-9]*) fail "budget field $key is not a non-negative integer" ;;
    esac
    printf '%s\n' "$value"
}

say "prove provider is enabled and expected durable budget state is current"
before=$(sh "$control_script" budget)
printf '%s\n' "$before"
[ "$(budget_value provider_enabled "$before")" = "true" ] \
    || fail "provider is not enabled"
before_total=$(numeric_budget_value total_used "$before")
[ "$before_total" -eq "$expected_before" ] \
    || fail "expected total_used=$expected_before but observed $before_total"
max_total=$(numeric_budget_value max_total "$before")
[ "$before_total" -lt "$max_total" ] || fail "lifetime provider budget is exhausted"
[ "$(budget_value next_new_call "$before")" = "available" ] \
    || fail "new provider call is not currently admissible"
before_last=$(budget_value last_consumed_at_ms "$before")

say "submit exactly one durable provider task"
submission=$(sh "$control_script" openai "$task_id" "$text")
printf '%s\n' "$submission"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-next-openai.XXXXXX")
cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM
report="$workspace/task.report"
printf '%s\n' "$submission" > "$report"

elapsed=0
while :; do
    status=$(sed -n 's/^status=//p' "$report" | tail -n 1)
    case "$status" in
        succeeded)
            break
            ;;
        failed|cancelled|manual_review)
            cat "$report" >&2
            fail "provider task reached terminal status $status"
            ;;
        pending|running|cancel_requested|'')
            ;;
        *)
            cat "$report" >&2
            fail "unexpected task status: $status"
            ;;
    esac

    [ "$elapsed" -lt "$max_wait_seconds" ] \
        || { cat "$report" >&2; fail "provider task did not become terminal in time"; }
    sleep "$poll_seconds"
    elapsed=$((elapsed + 1))
    sh "$control_script" task "$task_id" > "$report"
done

say "inspect successful durable result and normalized token usage"
cat "$report"
python3 - "$report" "$expected_model" "$task_id" <<'PY'
import json
import sys

path, expected_model, expected_task_id = sys.argv[1:4]
fields = {}
with open(path, encoding="utf-8") as source:
    for raw in source:
        raw = raw.rstrip("\n")
        if "=" in raw:
            key, value = raw.split("=", 1)
            fields[key] = value

try:
    actual_task_id = json.loads(fields.get("id", "null"))
except json.JSONDecodeError as exc:
    raise SystemExit(f"invalid task id report: {exc}")
if actual_task_id != expected_task_id:
    raise SystemExit(f"unexpected task id: {actual_task_id!r}")
if fields.get("kind") != '"provider.openai.responses"':
    raise SystemExit("unexpected provider task kind")
if fields.get("status") != "succeeded":
    raise SystemExit("task is not succeeded")
if fields.get("attempts") != "1/2":
    raise SystemExit("unexpected provider task attempt count")
if fields.get("result_metadata_content_type") != '"application/vnd.gaudere.provider-usage+json"':
    raise SystemExit("missing normalized provider usage content type")
if "result_metadata" not in fields:
    raise SystemExit("missing normalized provider usage metadata")

metadata_text = json.loads(fields["result_metadata"])
metadata = json.loads(metadata_text)
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

print("normalized_usage=PASS")
print(f"input_tokens={metadata['input_tokens']}")
print(f"cached_input_tokens={metadata['cached_input_tokens']}")
print(f"cache_write_input_tokens={metadata['cache_write_input_tokens']}")
print(f"output_tokens={metadata['output_tokens']}")
print(f"reasoning_tokens={metadata['reasoning_tokens']}")
print(f"total_tokens={metadata['total_tokens']}")
PY

say "prove durable provider budget advanced by exactly one lifetime permit"
after=$(sh "$control_script" budget)
printf '%s\n' "$after"
[ "$(budget_value provider_enabled "$after")" = "true" ] \
    || fail "provider unexpectedly became disabled"
after_total=$(numeric_budget_value total_used "$after")
expected_after=$((expected_before + 1))
[ "$after_total" -eq "$expected_after" ] \
    || fail "expected total_used=$expected_after after one call, observed $after_total"
after_max_total=$(numeric_budget_value max_total "$after")
[ "$after_max_total" -eq "$max_total" ] || fail "budget max_total changed during validation"
remaining_total=$(numeric_budget_value remaining_total "$after")
expected_remaining=$((max_total - expected_after))
[ "$remaining_total" -eq "$expected_remaining" ] \
    || fail "unexpected remaining_total after provider call"
after_last=$(budget_value last_consumed_at_ms "$after")
[ -n "$after_last" ] && [ "$after_last" != "none" ] \
    || fail "provider call did not record a durable consumption timestamp"
[ "$after_last" != "$before_last" ] \
    || fail "provider call did not advance durable consumption timestamp"
[ "$(budget_value next_new_call "$after")" = "cooldown" ] \
    || fail "expected cooldown immediately after provider call"

service_state=$($systemctl_command --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name is not active after provider call"

say "single additional permanent provider lifecycle proof complete"
printf 'gaudere next production OpenAI validation: PASS\n'
