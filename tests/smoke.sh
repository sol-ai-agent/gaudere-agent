#!/bin/sh
set -eu

temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM

state="$temporary_directory/state.db"
agent="../src/gaudere-agent"

"$agent" --state "$state" --check
test -f "$state"
test -f "$state.lock"

"$agent" --state "$state" --echo smoke-echo "hello gaudere" >"$temporary_directory/echo-output" 2>&1
grep -q "gaudere-agent: echo result: hello gaudere" "$temporary_directory/echo-output"
grep -q "gaudere-agent: safe" "$temporary_directory/echo-output"

# Reusing the same durable id is idempotent: the persisted result is returned
# rather than creating or executing a second task.
"$agent" --state "$state" --echo smoke-echo "different input" >"$temporary_directory/echo-repeat" 2>&1
grep -q "gaudere-agent: echo result: hello gaudere" "$temporary_directory/echo-repeat"
grep -q "gaudere-agent: safe" "$temporary_directory/echo-repeat"

"$agent" --state "$state" --task smoke-echo >"$temporary_directory/task-report" 2>&1
grep -q '^status=succeeded$' "$temporary_directory/task-report"
grep -q '^attempts=1/1$' "$temporary_directory/task-report"
grep -q '^result_output="hello gaudere"$' "$temporary_directory/task-report"

"$agent" --state "$state" >"$temporary_directory/output" 2>&1 &
agent_pid=$!
sleep 1

# Offline maintenance commands cannot race the live service for the same DB.
if "$agent" --state "$state" --task smoke-echo >"$temporary_directory/locked-report" 2>&1; then
    echo "offline inspection unexpectedly acquired a live state database" >&2
    exit 1
fi
grep -q "state database is already owned" "$temporary_directory/locked-report"

kill -TERM "$agent_pid"
wait "$agent_pid"

grep -q "gaudere-agent: running" "$temporary_directory/output"
grep -q "gaudere-agent: safe" "$temporary_directory/output"

# The same inspection succeeds once the service releases ownership.
"$agent" --state "$state" --task smoke-echo >"$temporary_directory/final-report" 2>&1
grep -q '^status=succeeded$' "$temporary_directory/final-report"
