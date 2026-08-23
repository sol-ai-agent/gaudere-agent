#!/bin/sh
set -eu

# Safe post-deadline proof for an isolated staging runtime that was deliberately
# stopped across due_at. This script starts/restarts/stops ONLY the staging
# service. It never powers off/reboots/logs out the host and never stops
# production.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script="$script_directory/control-service.sh"
systemctl_command=${SYSTEMCTL:-systemctl}
podman_command=${PODMAN:-podman}

production_service=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
production_container=${GAUDERE_CONTAINER:-gaudere-agent}
production_socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}
production_state_directory=${GAUDERE_STATE_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"}
production_database="$production_state_directory/state.db"
staging_service=${GAUDERE_WAKE_STAGING_SERVICE:-gaudere-wake-staging.service}
staging_container=${GAUDERE_WAKE_STAGING_CONTAINER:-gaudere-wake-staging}
staging_socket=${GAUDERE_WAKE_STAGING_SOCKET:-/tmp/gaudere-wake-staging-control.sock}
staging_root=${GAUDERE_WAKE_STAGING_ROOT:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-host-downtime-v0"}
staging_database="$staging_root/state/state.db"
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
staging_profile="$quadlet_directory/gaudere-wake-staging.container"
proof_root=${GAUDERE_WAKE_HOST_DOWNTIME_PROOF_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-proof-v0/host-downtime"}
boot_id_file=${GAUDERE_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
source_task=production-reflection-wake-source-first
frozen_runtime_image=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01

phase=preflight
fail()
{
    printf 'gaudere wake runtime-downtime observe: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

meta_value()
{
    file=$1
    key=$2
    sed -n "s/^${key}=//p" "$proof_root/$file" | tail -n 1
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

normalize_image_id()
{
    value=$1
    case "$value" in sha256:*) digest=${value#sha256:} ;; *) digest=$value ;; esac
    case "$digest" in *[!0-9a-f]*|'') return 1 ;; esac
    [ "${#digest}" -eq 64 ] || return 1
    printf 'sha256:%s\n' "$digest"
}

container_image()
{
    raw=$("$podman_command" container inspect --format '{{.Image}}' "$1" 2>/dev/null) || return 1
    normalize_image_id "$raw"
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

snapshot_database()
{
    db_path=$1
    output=$2
    mode=$3
    python3 - "$db_path" "$output" "$mode" <<'PY'
import base64, json, pathlib, sqlite3, sys
path, output, mode = sys.argv[1:]
uri = pathlib.Path(path).resolve().as_uri() + '?mode=ro'
def enc(v):
    return {'bytes_base64': base64.b64encode(v).decode('ascii')} if isinstance(v, bytes) else v
def qi(v):
    return '"' + v.replace('"','""') + '"'
with sqlite3.connect(uri, uri=True) as db:
    db.execute('PRAGMA query_only=ON')
    version = db.execute('PRAGMA user_version').fetchone()[0]
    if version != 4:
        raise SystemExit(f'schema is not 4: {version}')
    if [r[0] for r in db.execute('PRAGMA integrity_check')] != ['ok']:
        raise SystemExit('integrity_check failed')
    sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
    if mode == 'nonwake':
        sql += " AND name!='wake_intents'"
    sql += ' ORDER BY name'
    tables = [r[0] for r in db.execute(sql)]
    contents = {}
    for table in tables:
        cols = db.execute(f'PRAGMA table_xinfo({qi(table)})').fetchall()
        rows = [[enc(v) for v in row] for row in db.execute(f'SELECT * FROM {qi(table)}').fetchall()]
        rows.sort(key=lambda row: json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(',',':')))
        contents[table] = {'columns': cols, 'rows': rows}
    if mode == 'all':
        objects = db.execute("SELECT type,name,tbl_name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name").fetchall()
    else:
        objects = db.execute("SELECT type,name,tbl_name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND name!='wake_intents' AND tbl_name!='wake_intents' ORDER BY type,name").fetchall()
pathlib.Path(output).write_text(json.dumps({'schema':version,'objects':objects,'tables':contents}, ensure_ascii=False, sort_keys=True, separators=(',',':'))+'\n', encoding='utf-8')
PY
}

[ "$#" -eq 1 ] || fail "usage: $0 --observe-staging-after-due-and-close"
[ "$1" = "--observe-staging-after-due-and-close" ] || fail "explicit staging-only observation argument is required"
[ -f "$proof_root/phase-arm.meta" ] || fail "arm metadata missing"
[ -f "$proof_root/runtime-stop.meta" ] || fail "runtime stop witness missing"
[ -f "$proof_root/production-before.json" ] || fail "production baseline missing"
[ -f "$proof_root/staging-nonwake-baseline.json" ] || fail "staging non-wake baseline missing"
[ -f "$staging_database" ] || fail "staging database missing"
[ -f "$staging_profile" ] || fail "staging profile missing"
[ "$(meta_value phase-arm.meta phase_arm)" = "PASS" ] || fail "arm phase is not PASS"
[ "$(meta_value runtime-stop.meta runtime_stop)" = "PASS" ] || fail "runtime stop witness is not PASS"
[ "$(meta_value phase-arm.meta source_task)" = "$source_task" ] || fail "arm source mismatch"
[ "$(meta_value runtime-stop.meta source_task)" = "$source_task" ] || fail "stop source mismatch"

accepted_at=$(meta_value phase-arm.meta accepted_at_ms)
due_at=$(meta_value phase-arm.meta due_at_ms)
stopped_at=$(meta_value runtime-stop.meta stopped_observed_at_ms)
arm_boot_id=$(meta_value phase-arm.meta boot_id)
stop_boot_id=$(meta_value runtime-stop.meta boot_id)
current_boot_id=$(tr -d '\n' < "$boot_id_file")
[ "$stop_boot_id" = "$arm_boot_id" ] || fail "stop boot differs from ARM boot"
[ "$current_boot_id" = "$arm_boot_id" ] || fail "host rebooted; staging-only proof requires the same Fedora boot"
[ "$stopped_at" -lt "$due_at" ] || fail "staging stop was not observed before due"
[ "$(service_state "$staging_service")" != "active" ] || fail "staging service must remain inactive before overdue observation"
[ "$(service_state "$production_service")" = "active" ] || fail "production service is not active"

now_ms=$(python3 - <<'PY'
import time
print(time.time_ns() // 1_000_000)
PY
)
[ "$now_ms" -gt "$due_at" ] || fail "deadline has not passed yet"

phase=offline-overdue-proof
python3 - "$staging_database" "$source_task" "$accepted_at" "$due_at" <<'PY'
import pathlib, sqlite3, sys
path, source, accepted, due = sys.argv[1:]
uri = pathlib.Path(path).resolve().as_uri() + '?mode=ro'
with sqlite3.connect(uri, uri=True) as db:
    db.execute('PRAGMA query_only=ON')
    rows = db.execute('SELECT id,source_id,accepted_at_ms,due_at_ms,status,terminal_at_ms,terminal_reason FROM wake_intents ORDER BY rowid').fetchall()
expected = [(source, source, int(accepted), int(due), 0, None, '')]
if rows != expected:
    raise SystemExit(f'offline overdue row changed while runtime absent: {rows!r}')
PY

if production_wake=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake" >&2
    fail "production WakeIntent unexpectedly enabled"
fi
printf '%s\n' "$production_wake" | grep -q 'explicit wake capability is not enabled in this service' || fail "production wake-off invariant ambiguous"
production_budget=$(production_control budget)
[ "$(report_value total_used "$production_budget")" = "4" ] || fail "production provider total changed"

phase=start-and-reconcile
"$systemctl_command" --user start "$staging_service"
[ "$(service_state "$staging_service")" = "active" ] || fail "staging service did not start"
first=''
i=0
while [ "$i" -lt 100 ]; do
    if first=$(staging_control wake-status 2>/dev/null); then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
[ -n "$first" ] || fail "staging control socket did not become ready"
printf '%s\n' "$first" | tee "$proof_root/post-runtime-restart-first.report"
[ "$(report_value status "$first")" = "fired" ] || fail "overdue wake did not reconcile to fired"
[ "$(report_value source_task_id "$first")" = "\"$source_task\"" ] || fail "fired source mismatch"
[ "$(report_value accepted_at_ms "$first")" = "$accepted_at" ] || fail "accepted_at changed"
[ "$(report_value due_at_ms "$first")" = "$due_at" ] || fail "due_at changed"
terminal_at=$(report_value terminal_at_ms "$first")
case "$terminal_at" in ''|*[!0-9]*) fail "invalid terminal_at_ms" ;; esac
[ "$terminal_at" -gt "$due_at" ] || fail "runtime-downtime wake lacks positive lateness"
lateness=$((terminal_at - due_at))
[ "$(report_value health "$first")" = "terminal" ] || fail "wake health is not terminal"

second=$(staging_control wake-status) || fail "second wake observation failed"
[ "$(report_value terminal_at_ms "$second")" = "$terminal_at" ] || fail "second observation changed terminal timestamp"
[ "$(report_value status "$second")" = "fired" ] || fail "second observation changed terminal status"

phase=restart-idempotency
"$systemctl_command" --user restart "$staging_service"
[ "$(service_state "$staging_service")" = "active" ] || fail "staging controlled restart failed"
third=''
i=0
while [ "$i" -lt 100 ]; do
    if third=$(staging_control wake-status 2>/dev/null); then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
[ -n "$third" ] || fail "staging control socket did not return after restart"
[ "$(report_value terminal_at_ms "$third")" = "$terminal_at" ] || fail "restart changed terminal timestamp"
[ "$(report_value status "$third")" = "fired" ] || fail "restart changed terminal status"

snapshot_database "$staging_database" "$proof_root/staging-nonwake-after-runtime-restart.json" nonwake
cmp "$proof_root/staging-nonwake-baseline.json" "$proof_root/staging-nonwake-after-runtime-restart.json" >/dev/null || fail "runtime downtime changed staging non-wake durable state"

phase=production-invariant
[ "$(service_state "$production_service")" = "active" ] || fail "production service not active"
[ "$(container_image "$production_container")" = "$frozen_runtime_image" ] || fail "production runtime image changed"
production_budget=$(production_control budget)
[ "$(report_value total_used "$production_budget")" = "4" ] || fail "production provider total changed"
snapshot_database "$production_database" "$proof_root/production-after-runtime-downtime.json" all
cmp "$proof_root/production-before.json" "$proof_root/production-after-runtime-downtime.json" >/dev/null || fail "production durable state changed during isolated runtime proof"

phase=offline-final-proof
"$systemctl_command" --user stop "$staging_service"
[ "$(service_state "$staging_service")" != "active" ] || fail "staging service did not stop"
python3 - "$staging_database" "$source_task" "$accepted_at" "$due_at" "$terminal_at" <<'PY'
import sqlite3, sys
path, source, accepted, due, terminal = sys.argv[1:]
with sqlite3.connect(path) as db:
    rows = db.execute('SELECT id,source_id,accepted_at_ms,due_at_ms,status,terminal_at_ms,terminal_reason FROM wake_intents ORDER BY rowid').fetchall()
expected = [(source, source, int(accepted), int(due), 1, int(terminal), '')]
if rows != expected:
    raise SystemExit(f'final staged wake row mismatch: {rows!r}')
PY
python3 - "$staging_database" "$proof_root/staging-final-runtime-downtime.db" <<'PY'
import os, pathlib, sqlite3, sys
source, target = map(pathlib.Path, sys.argv[1:])
uri = source.resolve().as_uri() + '?mode=ro'
with sqlite3.connect(uri, uri=True) as src, sqlite3.connect(target) as dst:
    src.execute('PRAGMA query_only=ON')
    src.backup(dst)
os.chmod(target, 0o600)
PY

phase=cleanup
rm -f "$staging_profile"
"$systemctl_command" --user daemon-reload
rm -rf "$staging_root"
[ ! -e "$staging_profile" ] || fail "staging profile still exists"
[ ! -e "$staging_root" ] || fail "staging root still exists"

cat > "$proof_root/phase-runtime-observe.meta" <<EOF
phase_runtime_observe=PASS
source_task=$source_task
boot_id=$current_boot_id
stopped_observed_at_ms=$stopped_at
accepted_at_ms=$accepted_at
due_at_ms=$due_at
terminal_at_ms=$terminal_at
lateness_ms=$lateness
legacy_poweroff_attempt_ignored=YES
EOF
chmod 0600 "$proof_root/phase-runtime-observe.meta"

printf 'source_task=%s\n' "$source_task"
printf 'boot_id=%s\n' "$current_boot_id"
printf 'stopped_observed_at_ms=%s\n' "$stopped_at"
printf 'due_at_ms=%s\n' "$due_at"
printf 'terminal_at_ms=%s\n' "$terminal_at"
printf 'lateness_ms=%s\n' "$lateness"
printf 'runtime_down_across_deadline=PASS\n'
printf 'same_host_boot=PASS\n'
printf 'single_terminal_transition=PASS\n'
printf 'restart_idempotency=PASS\n'
printf 'nonwake_state_unchanged=PASS\n'
printf 'provider_effects=0\n'
printf 'successor_effects=0\n'
printf 'production_untouched=PASS\n'
printf 'production_provider_total_after=4\n'
printf 'staging_profile_removed=PASS\n'
printf 'host_action=none\n'
printf 'gaudere wake runtime-downtime observe: PASS\n'