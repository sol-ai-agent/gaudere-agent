#!/bin/sh
set -eu

# PREP ONLY. When separately authorized, this records a durable pre-deadline
# shutdown witness and requests a real host poweroff. It does not mutate either
# production or staging SQLite state and never submits provider work.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script="$script_directory/control-service.sh"
systemctl_command=${SYSTEMCTL:-systemctl}
poweroff_command=${GAUDERE_POWEROFF_COMMAND:-systemctl}

production_service=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
production_container=${GAUDERE_CONTAINER:-gaudere-agent}
production_socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}
staging_service=${GAUDERE_WAKE_STAGING_SERVICE:-gaudere-wake-staging.service}
staging_container=${GAUDERE_WAKE_STAGING_CONTAINER:-gaudere-wake-staging}
staging_socket=${GAUDERE_WAKE_STAGING_SOCKET:-/tmp/gaudere-wake-staging-control.sock}
proof_root=${GAUDERE_WAKE_HOST_DOWNTIME_PROOF_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-proof-v0/host-downtime"}
boot_id_file=${GAUDERE_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
source_task=production-reflection-wake-source-first
minimum_shutdown_margin_ms=${GAUDERE_WAKE_SHUTDOWN_MARGIN_MS:-120000}

phase=preflight
fail()
{
    printf 'gaudere wake host-downtime poweroff: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

report_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
}

meta_value()
{
    key=$1
    sed -n "s/^${key}=//p" "$proof_root/phase-arm.meta" | tail -n 1
}

service_state()
{
    "$systemctl_command" --user is-active "$1" 2>/dev/null || true
}

production_control()
{
    GAUDERE_CONTAINER="$production_container" GAUDERE_CONTROL_SOCKET="$production_socket" \
        sh "$control_script" "$@"
}

staging_control()
{
    GAUDERE_CONTAINER="$staging_container" GAUDERE_CONTROL_SOCKET="$staging_socket" \
        sh "$control_script" "$@"
}

[ "$#" -eq 1 ] || fail "usage: $0 --poweroff-after-explicit-host-downtime-go"
[ "$1" = "--poweroff-after-explicit-host-downtime-go" ] \
    || fail "explicit host poweroff authorization argument is required"
for command in python3 sed sync; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
command -v "$poweroff_command" >/dev/null 2>&1 || fail "poweroff command not found"
[ -r "$boot_id_file" ] || fail "boot-id source is not readable"
[ -f "$proof_root/phase-arm.meta" ] || fail "arm proof metadata is missing"
[ ! -e "$proof_root/poweroff.meta" ] || fail "poweroff witness already exists"
[ "$(meta_value phase_arm)" = "PASS" ] || fail "arm phase did not record PASS"
[ "$(meta_value source_task)" = "$source_task" ] || fail "arm source Task mismatch"

accepted_at=$(meta_value accepted_at_ms)
due_at=$(meta_value due_at_ms)
arm_boot_id=$(meta_value boot_id)
case "$accepted_at" in ''|*[!0-9]*) fail "invalid persisted accepted_at_ms" ;; esac
case "$due_at" in ''|*[!0-9]*) fail "invalid persisted due_at_ms" ;; esac
case "$minimum_shutdown_margin_ms" in ''|*[!0-9]*) fail "invalid shutdown margin" ;; esac
[ $((due_at - accepted_at)) -eq 3600000 ] || fail "persisted wake delay is not 3600 seconds"

current_boot_id=$(tr -d '\n' < "$boot_id_file")
[ "$current_boot_id" = "$arm_boot_id" ] || fail "host already rebooted since arm phase"
[ "$(service_state "$production_service")" = "active" ] || fail "production service is not active"
[ "$(service_state "$staging_service")" = "active" ] || fail "staging service is not active"

if production_wake=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake" >&2
    fail "production WakeIntent is unexpectedly enabled"
fi
printf '%s\n' "$production_wake" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "production wake capability invariant is ambiguous"
production_budget=$(production_control budget)
[ "$(report_value total_used "$production_budget")" = "4" ] || fail "production provider total changed"

staging_status=$(staging_control wake-status) || fail "cannot inspect staged WakeIntent"
printf '%s\n' "$staging_status"
[ "$(report_value status "$staging_status")" = "scheduled" ] || fail "staged WakeIntent is no longer scheduled"
[ "$(report_value source_task_id "$staging_status")" = "\"$source_task\"" ] || fail "staged WakeIntent source mismatch"
[ "$(report_value accepted_at_ms "$staging_status")" = "$accepted_at" ] || fail "staged accepted_at changed"
[ "$(report_value due_at_ms "$staging_status")" = "$due_at" ] || fail "staged due_at changed"

now_ms=$(python3 - <<'PY'
import time
print(time.time_ns() // 1_000_000)
PY
)
case "$now_ms" in ''|*[!0-9]*) fail "cannot determine current time" ;; esac
latest_safe=$((due_at - minimum_shutdown_margin_ms))
[ "$now_ms" -lt "$latest_safe" ] \
    || fail "too late to request a bounded pre-deadline shutdown safely"

phase=witness
umask 077
tmp="$proof_root/poweroff.meta.tmp.$$"
cat > "$tmp" <<EOF
poweroff_requested=YES
source_task=$source_task
boot_id=$current_boot_id
requested_at_ms=$now_ms
accepted_at_ms=$accepted_at
due_at_ms=$due_at
minimum_shutdown_margin_ms=$minimum_shutdown_margin_ms
EOF
mv "$tmp" "$proof_root/poweroff.meta"
sync

printf 'source_task=%s\n' "$source_task"
printf 'boot_id=%s\n' "$current_boot_id"
printf 'poweroff_requested_at_ms=%s\n' "$now_ms"
printf 'due_at_ms=%s\n' "$due_at"
printf 'remaining_before_due_ms=%s\n' $((due_at - now_ms))
printf 'production_provider_total=4\n'
printf 'staging_wake_status=scheduled\n'
printf 'status=POWER_OFF_REQUESTED_BEFORE_DUE\n'

phase=poweroff
if ! "$poweroff_command" poweroff; then
    fail "host poweroff request failed"
fi

printf 'poweroff_command_accepted=PASS\n'
