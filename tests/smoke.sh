#!/bin/sh
set -eu

temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM

state="$temporary_directory/state.db"
agent="../src/gaudere-agent"

"$agent" --state "$state" --check
test -f "$state"

"$agent" --state "$state" --echo smoke-echo "hello gaudere" >"$temporary_directory/echo-output" 2>&1
grep -q "gaudere-agent: echo result: hello gaudere" "$temporary_directory/echo-output"
grep -q "gaudere-agent: safe" "$temporary_directory/echo-output"

# Reusing the same durable id is idempotent: the persisted result is returned
# rather than creating or executing a second task.
"$agent" --state "$state" --echo smoke-echo "hello gaudere" >"$temporary_directory/echo-repeat" 2>&1
grep -q "gaudere-agent: echo result: hello gaudere" "$temporary_directory/echo-repeat"
grep -q "gaudere-agent: safe" "$temporary_directory/echo-repeat"

"$agent" --state "$state" >"$temporary_directory/output" 2>&1 &
agent_pid=$!
sleep 1
kill -TERM "$agent_pid"
wait "$agent_pid"

grep -q "gaudere-agent: running" "$temporary_directory/output"
grep -q "gaudere-agent: safe" "$temporary_directory/output"
