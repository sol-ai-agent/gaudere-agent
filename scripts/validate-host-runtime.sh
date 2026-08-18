#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/runtime.XXXXXX")
state_directory="$workspace/state"
container_name="gaudere-runtime-validation-$$"
mkdir -p "$state_directory"

cleanup()
{
    "$podman_command" rm -f "$container_name" >/dev/null 2>&1 || true
    if [ "${KEEP_GAUDERE_VALIDATION_STATE:-0}" = "1" ]; then
        printf 'validation state kept at %s\n' "$workspace" >&2
    else
        rm -rf "$workspace"
    fi
}
trap cleanup EXIT HUP INT TERM

say()
{
    printf '\n==> %s\n' "$*"
}

container_args()
{
    # This function exists only as documentation; POSIX sh cannot return argv.
    # Keep run_offline/start_service flags in sync with the hardened local shape.
    :
}

run_offline()
{
    "$podman_command" run --rm \
        --network none \
        --userns keep-id \
        --read-only \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$state_directory:/var/lib/gaudere:Z" \
        "$image" \
        --state /var/lib/gaudere/state.db "$@"
}

start_service()
{
    "$podman_command" run -d \
        --name "$container_name" \
        --network none \
        --userns keep-id \
        --read-only \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$state_directory:/var/lib/gaudere:Z" \
        "$image" \
        --state /var/lib/gaudere/state.db >/dev/null
}

remove_service()
{
    "$podman_command" rm -f "$container_name" >/dev/null 2>&1 || true
}

expect_line()
{
    text=$1
    pattern=$2
    if ! printf '%s\n' "$text" | grep -q "$pattern"; then
        printf 'validation failure: expected pattern %s\n' "$pattern" >&2
        printf '%s\n' "$text" >&2
        exit 1
    fi
}

if ! "$podman_command" image exists "$image"; then
    printf 'validation failure: image %s does not exist\n' "$image" >&2
    exit 1
fi

say "startup/check and durable echo"
run_offline --check >/dev/null
echo_output=$(run_offline --echo validation-echo "host validation")
expect_line "$echo_output" 'gaudere-agent: echo result: host validation'
expect_line "$echo_output" 'gaudere-agent: safe'

say "pending offline cancellation"
wait_pending=$(run_offline --enqueue-wait validation-pending-cancel 1000)
expect_line "$wait_pending" '^status=pending$'
cancel_output=$(run_offline --cancel validation-pending-cancel "host validation cancellation")
expect_line "$cancel_output" '^status=cancelled$'
expect_line "$cancel_output" '^cancel_reason="host validation cancellation"$'

say "exclusive state ownership while service is live"
start_service
sleep 0.3
if lock_output=$(run_offline --task validation-echo 2>&1); then
    printf 'validation failure: second process acquired live state database\n' >&2
    printf '%s\n' "$lock_output" >&2
    exit 1
fi
expect_line "$lock_output" 'state database is already owned'
"$podman_command" stop --time 5 "$container_name" >/dev/null
lock_service_logs=$("$podman_command" logs "$container_name" 2>&1)
expect_line "$lock_service_logs" 'gaudere-agent: safe'
remove_service

say "graceful SIGTERM cancellation of running local.wait"
run_offline --enqueue-wait validation-graceful 2000 >/dev/null
start_service
sleep 0.5
"$podman_command" stop --time 5 "$container_name" >/dev/null
graceful_logs=$("$podman_command" logs "$container_name" 2>&1)
expect_line "$graceful_logs" 'gaudere-agent: shutdown requested by signal 15'
expect_line "$graceful_logs" 'gaudere-agent: safe'
remove_service
graceful_report=$(run_offline --task validation-graceful)
expect_line "$graceful_report" '^status=cancelled$'
expect_line "$graceful_report" '^attempts=1/2$'
expect_line "$graceful_report" '^cancel_reason="worker shutdown requested"$'

say "hard SIGKILL and exact lease recovery"
run_offline --enqueue-wait validation-crash 500 >/dev/null
start_service
sleep 0.2
"$podman_command" kill --signal KILL "$container_name" >/dev/null
"$podman_command" wait "$container_name" >/dev/null || true
crash_report=$(run_offline --task validation-crash)
expect_line "$crash_report" '^status=running$'
expect_line "$crash_report" '^attempts=1/2$'
remove_service

# Restart promptly: the first 500 ms wait owns a 750 ms lease, so this replacement
# normally starts while that lease is still valid. It must not steal the lease early;
# WorkController schedules its exact expiry, then recovery consumes attempt two.
start_service
sleep 2
"$podman_command" stop --time 5 "$container_name" >/dev/null
recovery_logs=$("$podman_command" logs "$container_name" 2>&1)
expect_line "$recovery_logs" 'gaudere-agent: safe'
remove_service
recovered_report=$(run_offline --task validation-crash)
expect_line "$recovered_report" '^status=succeeded$'
expect_line "$recovered_report" '^attempts=2/2$'
expect_line "$recovered_report" '^result_output="waited 500 ms"$'

say "validation complete"
printf 'gaudere host runtime validation: PASS\n'
