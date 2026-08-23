#!/bin/sh
set -eu

# PREP ONLY. Phase B observes the already-accepted first real WakeIntent after its
# self-chosen deadline, proves the fired transition inert, then restores the exact
# wake-OFF Quadlet. It never submits provider work or another wake.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
control_script="$script_directory/control-service.sh"
podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
container_name=${GAUDERE_CONTAINER:-gaudere-agent}
state_directory=${GAUDERE_STATE_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"}
state_database="$state_directory/state.db"
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
profile="$quadlet_directory/gaudere-agent.container"
proof_root=${GAUDERE_FIRST_WAKE_PROOF_DIR:-"${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/wake-proof-v0/first-real-wake"}
source_task=production-reflection-wake-source-first
frozen_runtime_image=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
phase=preflight
success=0
profile_restored=0

fail()
{
    printf 'gaudere first real wake phase B: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

report_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
}

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

[ "$#" -eq 1 ] || fail "usage: $0 --observe-after-due-and-close"
[ "$1" = "--observe-after-due-and-close" ] || fail "explicit Phase B argument is required"
for command in awk cmp flock grep install python3 sed sha256sum tee; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
[ -f "$control_script" ] || fail "control helper not found"
[ -f "$state_database" ] || fail "state database not found"
[ -f "$profile" ] || fail "installed Quadlet profile not found"
[ -f "$proof_root/phase-a.meta" ] || fail "Phase A proof metadata is missing"
[ -f "$proof_root/profile.before" ] || fail "original wake-OFF profile proof is missing"
[ -f "$proof_root/profile.wake-enabled" ] || fail "wake-enabled profile proof is missing"
[ -f "$proof_root/nonwake-before.json" ] || fail "pre-acceptance durable snapshot is missing"

eval_meta()
{
    key=$1
    sed -n "s/^${key}=//p" "$proof_root/phase-a.meta" | tail -n 1
}
[ "$(eval_meta phase_a)" = "PASS" ] || fail "Phase A did not record PASS"
[ "$(eval_meta source_task)" = "$source_task" ] || fail "Phase A source Task mismatch"
[ "$(eval_meta runtime_image)" = "$frozen_runtime_image" ] || fail "Phase A runtime mismatch"
accepted_at=$(eval_meta accepted_at_ms)
due_at=$(eval_meta due_at_ms)
case "$accepted_at" in ''|*[!0-9]*) fail "invalid persisted accepted_at_ms" ;; esac
case "$due_at" in ''|*[!0-9]*) fail "invalid persisted due_at_ms" ;; esac
[ $((due_at - accepted_at)) -eq 3600000 ] || fail "Phase A persisted deadline delta is not 3600000 ms"

expected_profile_sha=$(eval_meta profile_wake_sha256)
actual_profile_sha=$(sha256sum "$profile" | awk '{print $1}')
[ "$actual_profile_sha" = "$expected_profile_sha" ] || fail "installed profile differs from Phase A wake-enabled profile"
[ "$(service_state)" = "active" ] || fail "$service_name must be active for due observation"
raw_image=$("$podman_command" container inspect --format '{{.Image}}' "$container_name" 2>/dev/null) \
    || fail "cannot inspect running image"
case "$raw_image" in sha256:*) current_image=$raw_image ;; *) current_image=sha256:$raw_image ;; esac
[ "$current_image" = "$frozen_runtime_image" ] || fail "running runtime image drift"

phase=due-observation
now_ms=$(python3 - <<'PY'
import time
print(time.time_ns() // 1_000_000)
PY
)
if [ "$now_ms" -lt "$due_at" ]; then
    remaining=$((due_at - now_ms))
    fail "too early; deadline is still ${remaining} ms away; no state changed"
fi

status=$(sh "$control_script" wake-status) || fail "wake-status is unhealthy after due"
printf '%s\n' "$status" | tee "$proof_root/status-phase-b.report"
[ "$(report_value record "$status")" = "one" ] || fail "wake record is not uniquely observable"
[ "$(report_value source_task_id "$status")" = '"production-reflection-wake-source-first"' ] || fail "terminal wake source mismatch"
[ "$(report_value status "$status")" = "fired" ] || fail "wake has not reached fired state"
[ "$(report_value health "$status")" = "terminal" ] || fail "fired wake is not reported terminal/healthy"
[ "$(report_value accepted_at_ms "$status")" = "$accepted_at" ] || fail "terminal accepted_at changed"
[ "$(report_value due_at_ms "$status")" = "$due_at" ] || fail "terminal due_at changed"
terminal_at=$(report_value terminal_at_ms "$status")
case "$terminal_at" in ''|*[!0-9]*) fail "invalid terminal_at_ms" ;; esac
[ "$terminal_at" -ge "$due_at" ] || fail "wake fired before its durable deadline"

budget=$(sh "$control_script" budget)
printf '%s\n' "$budget" | tee "$proof_root/budget-phase-b-before-close.report"
[ "$(report_value total_used "$budget")" = "4" ] || fail "provider budget changed during wake observation interval"

phase=offline-invariance
"$systemctl_command" --user stop "$service_name"
[ "$(service_state)" = "inactive" ] || fail "service did not stop for final durable proof"

recover()
{
    status_code=$?
    trap - EXIT HUP INT TERM
    [ "$success" = "1" ] && exit "$status_code"
    if [ "$profile_restored" = "1" ]; then
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
        "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
    fi
    printf 'gaudere first real wake phase B: proof incomplete; inspect %s before further wake work\n' "$proof_root" >&2
    exit "$status_code"
}
trap recover EXIT HUP INT TERM

exec 9>>"$state_database.lock"
chmod 600 "$state_database.lock" 2>/dev/null || true
flock -n 9 || fail "state database remains owned after service stop"
python3 - "$state_database" "$source_task" "$accepted_at" "$due_at" "$terminal_at" > "$proof_root/nonwake-after-fire.json" <<'PY'
import base64, json, pathlib, sqlite3, sys
path, source, accepted, due, terminal = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
uri = pathlib.Path(path).resolve().as_uri() + "?mode=ro"
def enc(v): return {"bytes_base64": base64.b64encode(v).decode("ascii")} if isinstance(v, bytes) else v
def qi(v): return '"' + v.replace('"','""') + '"'
with sqlite3.connect(uri, uri=True) as db:
    db.execute("PRAGMA query_only=ON")
    if db.execute("PRAGMA user_version").fetchone()[0] != 4: raise SystemExit("schema is not 4")
    if [r[0] for r in db.execute("PRAGMA integrity_check")] != ["ok"]: raise SystemExit("integrity_check failed")
    rows = db.execute("SELECT id,source_id,accepted_at_ms,due_at_ms,status,terminal_at_ms,terminal_reason FROM wake_intents ORDER BY rowid").fetchall()
    if rows != [(source, source, accepted, due, 1, terminal, "")]:
        raise SystemExit(f"unexpected final wake row: {rows!r}")
    blocker = db.execute("SELECT id,status FROM tasks WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1").fetchone()
    if blocker: raise SystemExit(f"nonterminal Task after fire: {blocker[0]}:{blocker[1]}")
    blocker = db.execute("SELECT id,status FROM actions WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1").fetchone()
    if blocker: raise SystemExit(f"nonterminal Action after fire: {blocker[0]}:{blocker[1]}")
    if db.execute("SELECT COUNT(*) FROM budget_consumptions WHERE scope=?", ("provider.call:openai.responses",)).fetchone()[0] != 4:
        raise SystemExit("provider consumption count changed")
    tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' AND name!='wake_intents' ORDER BY name")]
    contents = {}
    for table in tables:
        cols = db.execute(f"PRAGMA table_xinfo({qi(table)})").fetchall()
        rows2 = [[enc(v) for v in r] for r in db.execute(f"SELECT * FROM {qi(table)}").fetchall()]
        rows2.sort(key=lambda r: json.dumps(r, ensure_ascii=False, sort_keys=True, separators=(",",":")))
        contents[table] = {"columns":cols,"rows":rows2}
    objects = db.execute("SELECT type,name,tbl_name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND tbl_name!='wake_intents' ORDER BY type,name").fetchall()
print(json.dumps({"schema":4,"objects":objects,"tables":contents}, ensure_ascii=False, sort_keys=True, separators=(",",":")))
PY
flock -u 9
exec 9>&-

cmp -s "$proof_root/nonwake-before.json" "$proof_root/nonwake-after-fire.json" \
    || fail "non-wake durable state differs from pre-acceptance baseline"

phase=restore-wake-off
original_sha=$(eval_meta profile_before_sha256)
[ "$(sha256sum "$proof_root/profile.before" | awk '{print $1}')" = "$original_sha" ] \
    || fail "stored original profile hash mismatch"
install -m 0600 "$proof_root/profile.before" "$profile"
profile_restored=1
"$systemctl_command" --user daemon-reload
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "service did not restart on restored wake-OFF profile"

phase=final-wake-off-proof
if disabled=$(sh "$control_script" wake-status 2>&1); then
    printf '%s\n' "$disabled" >&2
    fail "wake-status unexpectedly succeeded after restoring wake-OFF profile"
fi
printf '%s\n' "$disabled" | tee "$proof_root/status-after-disable.report"
printf '%s\n' "$disabled" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "restored profile did not disable WakeIntent"
final_budget=$(sh "$control_script" budget)
printf '%s\n' "$final_budget" | tee "$proof_root/budget-final.report"
[ "$(report_value total_used "$final_budget")" = "4" ] || fail "provider budget changed during closeout"

cat > "$proof_root/phase-b.meta" <<EOF
source_task=$source_task
accepted_at_ms=$accepted_at
due_at_ms=$due_at
terminal_at_ms=$terminal_at
lateness_ms=$((terminal_at - due_at))
provider_total=4
wake_final=fired
wake_capability_final=false
phase_b=PASS
EOF
chmod 0600 "$proof_root/phase-b.meta"

success=1
trap - EXIT HUP INT TERM
printf 'source_task=%s\n' "$source_task"
printf 'accepted_at_ms=%s\n' "$accepted_at"
printf 'due_at_ms=%s\n' "$due_at"
printf 'terminal_at_ms=%s\n' "$terminal_at"
printf 'lateness_ms=%s\n' "$((terminal_at - due_at))"
printf 'wake_terminal=fired\n'
printf 'nonwake_state_unchanged=PASS\n'
printf 'provider_effects=0\n'
printf 'successor_effects=0\n'
printf 'wake_capability_active=false\n'
printf 'service_final=active\n'
printf 'provider_total_after=4\n'
printf 'gaudere first real wake phase B: PASS\n'
