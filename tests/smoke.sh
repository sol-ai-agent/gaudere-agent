#!/bin/sh
set -eu

temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM

state="$temporary_directory/state.db"
agent="../src/gaudere-agent"

"$agent" --state "$state" --check
test -f "$state"
test -f "$state.lock"

# Provider configuration can be preflighted without sending a network request.
# The secret value stays in a protected synthetic file; only its name is printed.
secret_directory="$temporary_directory/secrets"
mkdir "$secret_directory"
printf '%s' 'synthetic-openai-key' >"$secret_directory/smoke-openai-key"
chmod 0400 "$secret_directory/smoke-openai-key"
"$agent" --state "$state" --check \
  --openai-model gpt-test \
  --openai-secret smoke-openai-key \
  --secret-dir "$secret_directory" \
  >"$temporary_directory/provider-check" 2>&1
grep -q 'gaudere-agent: OpenAI provider enabled model=gpt-test secret=smoke-openai-key' \
  "$temporary_directory/provider-check"
grep -q 'gaudere-agent: safe' "$temporary_directory/provider-check"

# An OpenAI task can be queued offline. Without explicit provider activation the
# normal service does not recognize that kind, so it remains pending and unstarted.
"$agent" --state "$state" --enqueue-openai pending-openai "offline only" \
  >"$temporary_directory/openai-enqueue" 2>&1
grep -q '^status=pending$' "$temporary_directory/openai-enqueue"
grep -q '^attempts=0/2$' "$temporary_directory/openai-enqueue"

"$agent" --state "$state" >"$temporary_directory/openai-disabled-service" 2>&1 &
openai_disabled_pid=$!
sleep 0.3
kill -TERM "$openai_disabled_pid"
wait "$openai_disabled_pid"
grep -q 'gaudere-agent: safe' "$temporary_directory/openai-disabled-service"
"$agent" --state "$state" --task pending-openai \
  >"$temporary_directory/openai-pending-report" 2>&1
grep -q '^status=pending$' "$temporary_directory/openai-pending-report"
grep -q '^attempts=0/2$' "$temporary_directory/openai-pending-report"
"$agent" --state "$state" --cancel pending-openai "provider disabled smoke cleanup" \
  >"$temporary_directory/openai-cancel" 2>&1
grep -q '^status=cancelled$' "$temporary_directory/openai-cancel"

"$agent" --state "$state" --echo smoke-echo "hello gaudere" >"$temporary_directory/echo-output" 2>&1
grep -q "gaudere-agent: echo result: hello gaudere" "$temporary_directory/echo-output"
grep -q "gaudere-agent: safe" "$temporary_directory/echo-output"

# Reusing the same durable id is idempotent even when the proposed input changes.
"$agent" --state "$state" --echo smoke-echo "different input" >"$temporary_directory/echo-repeat" 2>&1
grep -q "gaudere-agent: echo result: hello gaudere" "$temporary_directory/echo-repeat"
grep -q "gaudere-agent: safe" "$temporary_directory/echo-repeat"

"$agent" --state "$state" --task smoke-echo >"$temporary_directory/task-report" 2>&1
grep -q '^status=succeeded$' "$temporary_directory/task-report"
grep -q '^attempts=1/1$' "$temporary_directory/task-report"
grep -q '^result_output="hello gaudere"$' "$temporary_directory/task-report"

# Pending work can be cancelled entirely offline before any worker starts it.
"$agent" --state "$state" --enqueue-wait pending-cancel 1000 >"$temporary_directory/pending" 2>&1
grep -q '^status=pending$' "$temporary_directory/pending"
"$agent" --state "$state" --cancel pending-cancel "smoke cancellation" >"$temporary_directory/pending-cancel" 2>&1
grep -q '^status=cancelled$' "$temporary_directory/pending-cancel"
grep -q '^cancel_reason="smoke cancellation"$' "$temporary_directory/pending-cancel"

# A graceful process stop is visible to a cooperative running handler without
# the signal thread mutating SQLite. The worker persists the acknowledged cancel.
"$agent" --state "$state" --enqueue-wait graceful-wait 2000 >"$temporary_directory/graceful-enqueue" 2>&1
"$agent" --state "$state" >"$temporary_directory/graceful-output" 2>&1 &
graceful_pid=$!
sleep 0.5
kill -TERM "$graceful_pid"
wait "$graceful_pid"
grep -q "gaudere-agent: safe" "$temporary_directory/graceful-output"
"$agent" --state "$state" --task graceful-wait >"$temporary_directory/graceful-report" 2>&1
grep -q '^status=cancelled$' "$temporary_directory/graceful-report"
grep -q '^cancel_reason="worker shutdown requested"$' "$temporary_directory/graceful-report"
grep -q '^attempts=1/2$' "$temporary_directory/graceful-report"

# Hard death leaves the durable running lease behind. A replacement process that
# starts before lease expiry must wait for the exact deadline, recover to pending,
# consume the second attempt, and complete without polling.
"$agent" --state "$state" --enqueue-wait crash-wait 500 >"$temporary_directory/crash-enqueue" 2>&1
"$agent" --state "$state" >"$temporary_directory/crash-first-output" 2>&1 &
crash_pid=$!
sleep 0.2
kill -KILL "$crash_pid"
wait "$crash_pid" || true

"$agent" --state "$state" --task crash-wait >"$temporary_directory/crash-running" 2>&1
grep -q '^status=running$' "$temporary_directory/crash-running"
grep -q '^attempts=1/2$' "$temporary_directory/crash-running"

"$agent" --state "$state" >"$temporary_directory/crash-recovery-output" 2>&1 &
recovery_pid=$!
sleep 2
kill -TERM "$recovery_pid"
wait "$recovery_pid"
grep -q "gaudere-agent: safe" "$temporary_directory/crash-recovery-output"

"$agent" --state "$state" --task crash-wait >"$temporary_directory/crash-done" 2>&1
grep -q '^status=succeeded$' "$temporary_directory/crash-done"
grep -q '^attempts=2/2$' "$temporary_directory/crash-done"
grep -q '^result_output="waited 500 ms"$' "$temporary_directory/crash-done"

# Offline maintenance commands cannot race the live service for the same DB.
"$agent" --state "$state" >"$temporary_directory/output" 2>&1 &
agent_pid=$!
sleep 0.5
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
