#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script=${GAUDERE_CONTROL_SCRIPT:-"$script_directory/control-service.sh"}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
expected_model=${GAUDERE_OPENAI_MODEL:-gpt-5.6-sol}
task_id=${1:-production-openai-first}
text=${2:-"Reponds uniquement avec les mots : Gaudere permanent."}
max_wait_seconds=${GAUDERE_VALIDATION_MAX_WAIT_SECONDS:-75}
poll_seconds=${GAUDERE_VALIDATION_POLL_SECONDS:-1}

fail()
{
    printf 'gaudere first production OpenAI validation: %s\n' "$*" >&2
    exit 1
}

[ -x "$control_script" ] || [ -f "$control_script" ] \
    || fail "control script not found: $control_script"
command -v "$systemctl_command" >/dev/null 2>&1 \
    || fail "systemctl command not found: $systemctl_command"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

case "$max_wait_seconds" in
    ''|*[!0-9]*) fail "GAUDERE_VALIDATION_MAX_WAIT_SECONDS must be an integer" ;;
esac
[ "$max_wait_seconds" -gt 0 ] || fail "wait timeout must be positive"

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

say "prove provider is enabled and permanent budget is pristine"
before=$(sh "$control_script" budget)
printf '%s\n' "$before"
[ "$(budget_value provider_enabled "$before")" = "true" ] \
    || fail "provider is not enabled"
[ "$(budget_value total_used "$before")" = "0" ] \
    || fail "first-call validator requires total_used=0"
[ "$(budget_value in_window_used "$before")" = "0" ] \
    || fail "first-call validator requires in_window_used=0"
[ "$(budget_value next_new_call "$before")" = "available" ] \
    || fail "new provider call is not currently admissible"

say "submit exactly one durable provider task"
submission=$(sh "$control_script" openai "$task_id" "$text")
printf '%s\n' "$submission"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-first-openai.XXXXXX")
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
python3 - "$report" "$expected_model" <<'PY'
import json
import sys

path, expected_model = sys.argv[1], sys.argv[2]
fields = {}
with open(path, encoding="utf-8") as source:
    for raw in source:
        raw = raw.rstrip("\n")
        if "=" in raw:
            key, value = raw.split("=", 1)
            fields[key] = value

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

say "prove exactly one permanent permit was consumed"
after=$(sh "$control_script" budget)
printf '%s\n' "$after"
[ "$(budget_value provider_enabled "$after")" = "true" ] \
    || fail "provider unexpectedly became disabled"
[ "$(budget_value total_used "$after")" = "1" ] \
    || fail "expected total_used=1 after first production call"
[ "$(budget_value remaining_total "$after")" = "11" ] \
    || fail "expected remaining_total=11"
[ "$(budget_value in_window_used "$after")" = "1" ] \
    || fail "expected in_window_used=1"
[ "$(budget_value remaining_window "$after")" = "3" ] \
    || fail "expected remaining_window=3"
[ "$(budget_value last_consumed_at_ms "$after")" != "none" ] \
    || fail "first production call did not record a durable consumption timestamp"
[ "$(budget_value next_new_call "$after")" = "cooldown" ] \
    || fail "expected cooldown immediately after first production call"

service_state=$($systemctl_command --user is-active "$service_name" 2>/dev/null || true)
[ "$service_state" = "active" ] || fail "$service_name is not active after provider call"

say "first permanent provider lifecycle proof complete"
printf 'gaudere first production OpenAI validation: PASS\n'
