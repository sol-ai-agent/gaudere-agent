#!/bin/sh
set -eu

# Safe staging-only proof gate. The staging service must already be stopped.
# This script never powers off/reboots/logs out the Fedora host and never stops
# production. It records that the isolated runtime is absent before due_at.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script="$script_directory/control-service.sh"
systemctl_command=${SYSTEMCTL:-systemctl}

production_service=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
production_container=${GAUDERE_CONTAINER:-gaudere-agent}
production_socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}
staging_service=${GAUDERE_WAKE_STAGING_SERVICE:-gaudere-wake-staging.service}
staging_root=${GAUDERE_WAKE_STAGING_ROOT:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-host-downtime-v0"}
staging_database="$staging_root/state/state.db"
proof_root=${GAUDERE_WAKE_HOST_DOWNTIME_PROOF_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-proof-v0/host-downtime"}
boot_id_file=${GAUDERE_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
source_task=production-reflection-wake-source-first

fail()
{
    printf 'gaudere wake runtime-downtime stop: %s\n' "$*" >&2
    exit 1
}

meta_value()
{
    key=$1
    sed -n "s/^${key}=//p" "$proof_root/phase-arm.meta" | tail -n 1
}

report_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
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

[ "$#" -eq 1 ] || fail "usage: $0 --record-staging-stop-before-due"
[ "$1" = "--record-staging-stop-before-due" ] || fail "explicit staging-only stop witness argument is required"
[ -f "$proof_root/phase-arm.meta" ] || fail "arm metadata missing"
[ ! -e "$proof_root/runtime-stop.meta" ] || fail "runtime stop witness already exists"
[ -f "$staging_database" ] || fail "staging database missing"
[ -r "$boot_id_file" ] || fail "boot id unreadable"
[ "$(meta_value phase_arm)" = "PASS" ] || fail "arm phase is not PASS"
[ "$(meta_value source_task)" = "$source_task" ] || fail "source mismatch"
[ "$(service_state "$production_service")" = "active" ] || fail "production service is not active"
[ "$(service_state "$staging_service")" != "active" ] || fail "staging service must already be inactive"

if production_wake=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake" >&2
    fail "production WakeIntent unexpectedly enabled"
fi
printf '%s\n' "$production_wake" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "production wake-off invariant is ambiguous"
production_budget=$(production_control budget)
[ "$(report_value total_used "$production_budget")" = "4" ] || fail "production provider total changed"

accepted_at=$(meta_value accepted_at_ms)
due_at=$(meta_value due_at_ms)
arm_boot_id=$(meta_value boot_id)
current_boot_id=$(tr -d '\n' < "$boot_id_file")
[ "$current_boot_id" = "$arm_boot_id" ] || fail "host rebooted since ARM; this is not the staging-only proof"

now_ms=$(python3 - <<'PY'
import time
print(time.time_ns() // 1_000_000)
PY
)
[ "$now_ms" -lt "$due_at" ] || fail "deadline already passed before the inactive-runtime witness"

python3 - "$staging_database" "$source_task" "$accepted_at" "$due_at" <<'PY'
import pathlib, sqlite3, sys
path, source, accepted, due = sys.argv[1:]
uri = pathlib.Path(path).resolve().as_uri() + '?mode=ro'
with sqlite3.connect(uri, uri=True) as db:
    db.execute('PRAGMA query_only=ON')
    rows = db.execute(
        'SELECT id,source_id,accepted_at_ms,due_at_ms,status,terminal_at_ms,terminal_reason '
        'FROM wake_intents ORDER BY rowid'
    ).fetchall()
expected = [(source, source, int(accepted), int(due), 0, None, '')]
if rows != expected:
    raise SystemExit(f'expected one scheduled inert wake row, got {rows!r}')
PY

umask 077
tmp="$proof_root/runtime-stop.meta.tmp.$$"
cat > "$tmp" <<EOF
runtime_stop=PASS
source_task=$source_task
boot_id=$current_boot_id
stopped_observed_at_ms=$now_ms
accepted_at_ms=$accepted_at
due_at_ms=$due_at
production_provider_total=4
staging_service_state=inactive
host_action=none
EOF
mv "$tmp" "$proof_root/runtime-stop.meta"
sync

printf 'runtime_stop=PASS\n'
printf 'source_task=%s\n' "$source_task"
printf 'boot_id=%s\n' "$current_boot_id"
printf 'stopped_observed_at_ms=%s\n' "$now_ms"
printf 'due_at_ms=%s\n' "$due_at"
printf 'remaining_before_due_ms=%s\n' $((due_at - now_ms))
printf 'staging_service_state=inactive\n'
printf 'production_service_state=active\n'
printf 'production_provider_total=4\n'
printf 'host_action=none\n'
printf 'gaudere wake runtime-downtime stop: PASS\n'