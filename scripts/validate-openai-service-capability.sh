#!/bin/sh
set -eu

service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
expected_total=${GAUDERE_EXPECT_BUDGET_TOTAL:-0}
expected_window=${GAUDERE_EXPECT_BUDGET_WINDOW:-0}
task_id=${1:-}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script="$script_directory/control-service.sh"

fail()
{
    printf 'gaudere OpenAI service capability validation: %s\n' "$*" >&2
    exit 1
}

expect_line()
{
    text=$1
    pattern=$2
    printf '%s\n' "$text" | grep -Eq "$pattern" \
        || fail "expected line matching: $pattern"
}

command -v systemctl >/dev/null 2>&1 || fail "systemctl is required"
[ -f "$control_script" ] || fail "control helper not found: $control_script"
[ "$(systemctl --user is-active "$service_name" 2>/dev/null || true)" = "active" ] \
    || fail "$service_name is not active"

printf '\n==> observe provider capability without submitting work\n'
budget=$(sh "$control_script" budget)
printf '%s\n' "$budget"
expect_line "$budget" '^provider_enabled=true$'
expect_line "$budget" "^total_used=$expected_total$"
expect_line "$budget" "^in_window_used=$expected_window$"
expect_line "$budget" '^max_total=12$'
expect_line "$budget" '^max_window=4$'
expect_line "$budget" '^min_interval_seconds=900$'

if [ -n "$task_id" ]; then
    printf '\n==> inspect existing durable task through the live owner\n'
    task=$(sh "$control_script" task "$task_id")
    printf '%s\n' "$task"
    expect_line "$task" '^status=succeeded$'
fi

printf '\n==> prove service remained live and no provider task was submitted by this validator\n'
[ "$(systemctl --user is-active "$service_name" 2>/dev/null || true)" = "active" ] \
    || fail "$service_name stopped during validation"
after=$(sh "$control_script" budget)
expect_line "$after" '^provider_enabled=true$'
expect_line "$after" "^total_used=$expected_total$"
expect_line "$after" "^in_window_used=$expected_window$"

printf 'gaudere OpenAI service capability validation: PASS\n'
