#!/bin/sh
set -eu

# PREP ONLY. After a separately authorized real host poweroff spanning the staged
# deadline, this phase proves reboot/autostart reconciliation, inert at-most-once
# firing, production invariance, then removes the isolated staging service.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script="$script_directory/control-service.sh"
podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}

production_service=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
production_container=${GAUDERE_CONTAINER:-gaudere-agent}
production_socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}
production_state_directory=${GAUDERE_STATE_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"}
production_database="$production_state_directory/state.db"

staging_service=${GAUDERE_WAKE_STAGING_SERVICE:-gaudere-wake-staging.service}
staging_container=${GAUDERE_WAKE_STAGING_CONTAINER:-gaudere-wake-staging}
staging_socket=${GAUDERE_WAKE_STAGING_SOCKET:-/tmp/gaudere-wake-staging-control.sock}
staging_root=${GAUDERE_WAKE_STAGING_ROOT:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-host-downtime-v0"}
staging_state_directory="$staging_root/state"
staging_database="$staging_state_directory/state.db"
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
staging_profile="$quadlet_directory/gaudere-wake-staging.container"
proof_root=${GAUDERE_WAKE_HOST_DOWNTIME_PROOF_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-proof-v0/host-downtime"}

boot_id_file=${GAUDERE_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
proc_stat_file=${GAUDERE_PROC_STAT_FILE:-/proc/stat}
source_task=production-reflection-wake-source-first
frozen_runtime_image=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
phase=preflight
success=0

fail()
{
    printf 'gaudere wake host-downtime observe: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

report_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
}

arm_value()
{
    key=$1
    sed -n "s/^${key}=//p" "$proof_root/phase-arm.meta" | tail -n 1
}

poweroff_value()
{
    key=$1
    sed -n "s/^${key}=//p" "$proof_root/poweroff.meta" | tail -n 1
}

normalize_image_id()
{
    value=$1
    case "$value" in sha256:*) digest=${value#sha256:} ;; *) digest=$value ;; esac
    case "$digest" in *[!0-9a-f]*|'') return 1 ;; esac
    [ "${#digest}" -eq 64 ] || return 1
    printf 'sha256:%s\n' "$digest"
}

service_state()
{
    "$systemctl_command" --user is-active "$1" 2>/dev/null || true
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
if mode not in {"all", "nonwake"}:
    raise SystemExit("invalid snapshot mode")
uri = pathlib.Path(path).resolve().as_uri() + "?mode=ro"
def enc(v):
    return {"bytes_base64": base64.b64encode(v).decode("ascii")} if isinstance(v, bytes) else v
def qi(v):
    return '"' + v.replace('"','""') + '"'
with sqlite3.connect(uri, uri=True) as db:
    db.execute("PRAGMA query_only=ON")
    version = db.execute("PRAGMA user_version").fetchone()[0]
    if version != 4:
        raise SystemExit(f"schema is not 4: {version}")
    if [r[0] for r in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("integrity_check failed")
    table_sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
    if mode == "nonwake":
        table_sql += " AND name!='wake_intents'"
    table_sql += " ORDER BY name"
    tables = [r[0] for r in db.execute(table_sql)]
    contents = {}
    for table in tables:
        cols = db.execute(f"PRAGMA table_xinfo({qi(table)})").fetchall()
        rows = [[enc(v) for v in row] for row in db.execute(f"SELECT * FROM {qi(table)}").fetchall()]
        rows.sort(key=lambda row: json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(",",":")))
        contents[table] = {"columns": cols, "rows": rows}
    if mode == "all":
        objects = db.execute(
            "SELECT type,name,tbl_name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"
        ).fetchall()
    else:
        objects = db.execute(
            "SELECT type,name,tbl_name,sql FROM sqlite_master "
            "WHERE name NOT LIKE 'sqlite_%' AND name!='wake_intents' "
            "AND tbl_name!='wake_intents' ORDER BY type,name"
        ).fetchall()
payload = {"schema": version, "objects": objects, "tables": contents}
pathlib.Path(output).write_text(
    json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",",":")) + "\n",
    encoding="utf-8",
)
PY
}

[ "$#" -eq 1 ] || fail "usage: $0 --observe-after-reboot-and-close"
[ "$1" = "--observe-after-reboot-and-close" ] || fail "explicit post-reboot observation argument is required"
for command in awk cmp grep python3 sed sha256sum; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
[ -f "$control_script" ] || fail "control helper not found"
[ -r "$boot_id_file" ] || fail "boot-id source is not readable"
[ -r "$proc_stat_file" ] || fail "proc stat source is not readable"
[ -f "$proof_root/phase-arm.meta" ] || fail "arm proof metadata is missing"
[ -f "$proof_root/poweroff.meta" ] || fail "pre-deadline poweroff witness is missing"
[ -f "$proof_root/production-before.json" ] || fail "production baseline is missing"
[ -f "$proof_root/staging-nonwake-baseline.json" ] || fail "staging non-wake baseline is missing"
[ -f "$staging_database" ] || fail "staging database is missing"
[ -f "$staging_profile" ] || fail "staging Quadlet is missing"
[ "$(arm_value phase_arm)" = "PASS" ] || fail "arm phase did not record PASS"
[ "$(poweroff_value poweroff_requested)" = "YES" ] || fail "poweroff witness is invalid"
[ "$(arm_value source_task)" = "$source_task" ] || fail "arm source Task mismatch"
[ "$(poweroff_value source_task)" = "$source_task" ] || fail "poweroff source Task mismatch"
[ "$(arm_value runtime_image)" = "$frozen_runtime_image" ] || fail "arm runtime image mismatch"

accepted_at=$(arm_value accepted_at_ms)
due_at=$(arm_value due_at_ms)
arm_boot_id=$(arm_value boot_id)
requested_at=$(poweroff_value requested_at_ms)
poweroff_boot_id=$(poweroff_value boot_id)
case "$accepted_at" in ''|*[!0-9]*) fail "invalid accepted_at_ms" ;; esac
case "$due_at" in ''|*[!0-9]*) fail "invalid due_at_ms" ;; esac
case "$requested_at" in ''|*[!0-9]*) fail "invalid poweroff requested_at_ms" ;; esac
[ $((due_at - accepted_at)) -eq 3600000 ] || fail "persisted deadline delta is not 3600000 ms"
[ "$requested_at" -lt "$due_at" ] || fail "poweroff was not requested before due"
[ "$arm_boot_id" = "$poweroff_boot_id" ] || fail "poweroff witness boot differs from arm boot"

phase=reboot-proof
current_boot_id=$(tr -d '\n' < "$boot_id_file")
[ "$current_boot_id" != "$arm_boot_id" ] || fail "boot id did not change; no reboot proof"
boot_start_seconds=$(awk '$1=="btime" {print $2; exit}' "$proc_stat_file")
case "$boot_start_seconds" in ''|*[!0-9]*) fail "cannot determine current boot start" ;; esac
boot_start_ms=$((boot_start_seconds * 1000))
[ "$boot_start_ms" -gt "$due_at" ] || fail "current host boot did not start after the staged due time"

profile_sha=$(sha256sum "$staging_profile" | awk '{print $1}')
[ "$profile_sha" = "$(arm_value staging_profile_sha256)" ] || fail "staging profile changed across reboot"
# Do not start staging here. Requiring active before any mutation is the host-level
# proof that the Quadlet returned with the user manager after reboot.
[ "$(service_state "$staging_service")" = "active" ] || fail "staging service did not autostart after reboot"
[ "$(container_image "$staging_container")" = "$frozen_runtime_image" ] || fail "staging runtime image changed across reboot"

phase=fired-observation
first=$(staging_control wake-status) || fail "cannot inspect staged WakeIntent after reboot"
printf '%s\n' "$first" | tee "$proof_root/post-reboot-first.report"
[ "$(report_value status "$first")" = "fired" ] || fail "staged WakeIntent did not reconcile to fired"
[ "$(report_value source_task_id "$first")" = "\"$source_task\"" ] || fail "fired source mismatch"
[ "$(report_value accepted_at_ms "$first")" = "$accepted_at" ] || fail "fired accepted_at changed"
[ "$(report_value due_at_ms "$first")" = "$due_at" ] || fail "fired due_at changed"
terminal_at=$(report_value terminal_at_ms "$first")
case "$terminal_at" in ''|*[!0-9]*) fail "invalid terminal_at_ms" ;; esac
[ "$terminal_at" -ge "$due_at" ] || fail "wake fired before due"
lateness=$((terminal_at - due_at))
[ "$lateness" -gt 0 ] || fail "host-downtime wake did not record positive lateness"
[ "$(report_value health "$first")" = "terminal" ] || fail "fired wake health is not terminal"

second=$(staging_control wake-status) || fail "second wake observation failed"
[ "$(report_value terminal_at_ms "$second")" = "$terminal_at" ] || fail "repeated observation changed terminal timestamp"
[ "$(report_value status "$second")" = "fired" ] || fail "repeated observation changed terminal status"

phase=restart-idempotency
"$systemctl_command" --user restart "$staging_service"
[ "$(service_state "$staging_service")" = "active" ] || fail "staging service failed controlled post-reboot restart"
third=$(staging_control wake-status) || fail "wake observation failed after controlled restart"
[ "$(report_value terminal_at_ms "$third")" = "$terminal_at" ] || fail "restart changed terminal timestamp"
[ "$(report_value status "$third")" = "fired" ] || fail "restart changed terminal status"

snapshot_database "$staging_database" "$proof_root/staging-nonwake-after-reboot.json" nonwake
cmp "$proof_root/staging-nonwake-baseline.json" "$proof_root/staging-nonwake-after-reboot.json" >/dev/null \
    || fail "host downtime/restart changed staging non-wake durable state"

phase=production-invariant
[ "$(service_state "$production_service")" = "active" ] || fail "production service is not active after reboot"
[ "$(container_image "$production_container")" = "$frozen_runtime_image" ] || fail "production runtime image changed"
if production_wake=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake" >&2
    fail "production WakeIntent is unexpectedly enabled"
fi
printf '%s\n' "$production_wake" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "production wake capability invariant is ambiguous"
production_budget=$(production_control budget)
[ "$(report_value total_used "$production_budget")" = "4" ] || fail "production provider total changed"
snapshot_database "$production_database" "$proof_root/production-after-reboot.json" all
cmp "$proof_root/production-before.json" "$proof_root/production-after-reboot.json" >/dev/null \
    || fail "production durable state changed during isolated host-downtime proof"

phase=offline-final-proof
"$systemctl_command" --user stop "$staging_service"
[ "$(service_state "$staging_service")" != "active" ] || fail "staging service did not stop"
python3 - "$staging_database" "$source_task" "$accepted_at" "$due_at" "$terminal_at" <<'PY'
import sqlite3, sys
path, source, accepted, due, terminal = sys.argv[1:]
with sqlite3.connect(path) as db:
    row = db.execute(
        "SELECT id,source_id,accepted_at_ms,due_at_ms,status,terminal_at_ms,terminal_reason "
        "FROM wake_intents ORDER BY rowid"
    ).fetchall()
expected = [(source, source, int(accepted), int(due), 1, int(terminal), "")]
if row != expected:
    raise SystemExit(f"final staged wake row mismatch: {row!r}")
PY
python3 - "$staging_database" "$proof_root/staging-final.db" <<'PY'
import os, pathlib, sqlite3, sys
source, target = map(pathlib.Path, sys.argv[1:])
uri = source.resolve().as_uri() + "?mode=ro"
with sqlite3.connect(uri, uri=True) as src, sqlite3.connect(target) as dst:
    src.execute("PRAGMA query_only=ON")
    src.backup(dst)
os.chmod(target, 0o600)
PY

phase=cleanup
rm -f "$staging_profile"
"$systemctl_command" --user daemon-reload
rm -rf "$staging_root"
[ ! -e "$staging_profile" ] || fail "staging profile still exists"
[ ! -e "$staging_root" ] || fail "staging state still exists"
[ "$(service_state "$production_service")" = "active" ] || fail "production service not active after staging cleanup"
if production_wake_final=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake_final" >&2
    fail "production WakeIntent enabled after staging cleanup"
fi
printf '%s\n' "$production_wake_final" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "production wake capability final state is ambiguous"
production_budget_final=$(production_control budget)
[ "$(report_value total_used "$production_budget_final")" = "4" ] || fail "production provider total changed during cleanup"

cat > "$proof_root/phase-observe.meta" <<EOF
phase_observe=PASS
source_task=$source_task
arm_boot_id=$arm_boot_id
reboot_boot_id=$current_boot_id
poweroff_requested_at_ms=$requested_at
boot_start_ms=$boot_start_ms
accepted_at_ms=$accepted_at
due_at_ms=$due_at
terminal_at_ms=$terminal_at
lateness_ms=$lateness
EOF
chmod 0600 "$proof_root/phase-observe.meta"

success=1
printf 'source_task=%s\n' "$source_task"
printf 'arm_boot_id=%s\n' "$arm_boot_id"
printf 'reboot_boot_id=%s\n' "$current_boot_id"
printf 'poweroff_requested_at_ms=%s\n' "$requested_at"
printf 'boot_start_ms=%s\n' "$boot_start_ms"
printf 'due_at_ms=%s\n' "$due_at"
printf 'terminal_at_ms=%s\n' "$terminal_at"
printf 'lateness_ms=%s\n' "$lateness"
printf 'host_down_across_deadline=PASS\n'
printf 'staging_autostart_after_reboot=PASS\n'
printf 'single_terminal_transition=PASS\n'
printf 'nonwake_state_unchanged=PASS\n'
printf 'provider_effects=0\n'
printf 'successor_effects=0\n'
printf 'production_untouched=PASS\n'
printf 'production_wake_capability_active=false\n'
printf 'production_provider_total_after=4\n'
printf 'staging_profile_removed=PASS\n'
printf 'production_service_final=active\n'
printf 'gaudere wake host-downtime observe: PASS\n'
