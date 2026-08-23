#!/bin/sh
set -eu

# PREP ONLY until Bertrand gives a separate explicit GO for the first real WakeIntent.
# Phase A enables the already-implemented inert WakeIntent capability, accepts exactly
# one frozen source, proves duplicate idempotency and restart re-arm, then leaves the
# service active until the self-chosen deadline. It never submits provider work.

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
expected_delay_seconds=3600
expected_delay_ms=3600000
expected_reason='Resume after a one-hour production observation window to verify that the active pre-wake runtime leaves durable, interpretable evidence, journal the result, and identify the single reliability condition that should gate any future WakeIntent enablement. This advances cooperation reliability without spending scarce provider budget on a more ambitious step.'
frozen_runtime_image=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
frozen_agent_ref=4e6cb09467456f38377bd8610e1ac534c7705380
frozen_core_ref=1316cf68db93e4c91a7bd79fbd289b8f382f8659
phase=preflight
profile_changed=0
wake_accepted=0
success=0

fail()
{
    printf 'gaudere first real wake phase A: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

normalize_image_id()
{
    value=$1
    case "$value" in sha256:*) digest=${value#sha256:} ;; *) digest=$value ;; esac
    case "$digest" in *[!0-9a-f]*|'') return 1 ;; esac
    [ "${#digest}" -eq 64 ] || return 1
    printf 'sha256:%s\n' "$digest"
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

running_image()
{
    raw=$("$podman_command" container inspect --format '{{.Image}}' "$container_name" 2>/dev/null) || return 1
    normalize_image_id "$raw"
}

[ "$#" -eq 1 ] || fail "usage: $0 --execute-after-explicit-first-wake-go"
[ "$1" = "--execute-after-explicit-first-wake-go" ] \
    || fail "explicit first-wake authorization argument is required"

for command in awk cmp cp date flock grep install mkdir mktemp mv python3 rm sed sha256sum tee; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
[ -f "$control_script" ] || fail "control helper not found"
[ -f "$state_database" ] || fail "production state database not found"
[ -f "$profile" ] || fail "installed Quadlet profile not found"
[ ! -e "$proof_root" ] || fail "proof directory already exists; inspect previous attempt: $proof_root"
[ "$(service_state)" = "active" ] || fail "$service_name must be active"

actual_image=$(running_image) || fail "cannot resolve running image"
[ "$actual_image" = "$frozen_runtime_image" ] || fail "running image differs from frozen baseline"
agent_ref=$("$podman_command" image inspect --format '{{index .Labels "io.gaudere.agent.revision"}}' "$actual_image" 2>/dev/null) \
    || fail "cannot inspect Agent provenance"
core_ref=$("$podman_command" image inspect --format '{{index .Labels "io.gaudere.core.revision"}}' "$actual_image" 2>/dev/null) \
    || fail "cannot inspect Core provenance"
[ "$agent_ref" = "$frozen_agent_ref" ] || fail "runtime Agent provenance drift"
[ "$core_ref" = "$frozen_core_ref" ] || fail "runtime Core provenance drift"

if wake_disabled=$(sh "$control_script" wake-status 2>&1); then
    printf '%s\n' "$wake_disabled" >&2
    fail "WakeIntent is already enabled before Phase A"
fi
printf '%s\n' "$wake_disabled" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "wake-status did not prove disabled capability"

budget_before=$(sh "$control_script" budget)
printf '%s\n' "$budget_before"
[ "$(report_value provider_enabled "$budget_before")" = "true" ] || fail "provider capability is not enabled"
[ "$(report_value total_used "$budget_before")" = "4" ] || fail "provider total must be exactly 4"
[ "$(report_value remaining_total "$budget_before")" = "8" ] || fail "provider remaining total must be 8"

source_report=$(sh "$control_script" task "$source_task") || fail "canonical source Task is not inspectable"
printf '%s\n' "$source_report"
SOURCE_REPORT="$source_report" python3 - "$source_task" "$expected_reason" "$expected_delay_seconds" <<'PY'
import json, os, sys
source_task, expected_reason, expected_delay = sys.argv[1], sys.argv[2], int(sys.argv[3])
fields = {}
for raw in os.environ["SOURCE_REPORT"].splitlines():
    if "=" in raw:
        k, v = raw.split("=", 1)
        fields[k] = v
if json.loads(fields.get("id", '""')) != source_task:
    raise SystemExit("source Task ID mismatch")
if json.loads(fields.get("kind", '""')) != "cognition.reflect.v1" or fields.get("status") != "succeeded":
    raise SystemExit("source Task is not a succeeded bounded reflection")
if json.loads(fields.get("result_content_type", '""')) != "application/vnd.gaudere.cognition-decision+json":
    raise SystemExit("source result content type mismatch")
result = json.loads(json.loads(fields["result_output"]))
expected = {
    "schema": "gaudere.cognition.decision.v1",
    "decision": "propose_wake",
    "reason": expected_reason,
    "wake_after_seconds": expected_delay,
}
if result != expected:
    raise SystemExit("source decision differs from frozen canonical proposal")
PY

phase=profile-preflight
python3 - "$profile" "$frozen_runtime_image" <<'PY'
import pathlib, sys
path, image = pathlib.Path(sys.argv[1]), sys.argv[2]
lines = path.read_text(encoding="utf-8").splitlines()
image_lines = [x for x in lines if x.startswith("Image=")]
exec_lines = [x for x in lines if x.startswith("Exec=")]
if image_lines != ["Image=" + image]:
    raise SystemExit("installed profile does not pin the frozen image")
if len(exec_lines) != 1:
    raise SystemExit("installed profile must contain exactly one Exec line")
exec_line = exec_lines[0]
if "--wake-intents" in exec_line:
    raise SystemExit("installed profile already enables WakeIntent")
for token in ("--control-socket", "--openai-model gpt-5.6-sol", "--openai-secret"):
    if token not in exec_line:
        raise SystemExit("installed profile lost required OpenAI/control configuration: " + token)
PY

install -d -m 0700 "$(dirname "$proof_root")"
mkdir -m 0700 "$proof_root"
printf '%s\n' "$source_report" > "$proof_root/source-task.report"
printf '%s\n' "$budget_before" > "$proof_root/budget-before.report"
install -m 0600 "$profile" "$proof_root/profile.before"

recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    [ "$success" = "1" ] && exit "$status"
    if [ "$wake_accepted" = "1" ]; then
        "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
        printf 'gaudere first real wake phase A: wake slot may be consumed; service left stopped for manual review\n' >&2
        printf 'gaudere first real wake phase A: wake-enabled profile and proof directory retained: %s\n' "$proof_root" >&2
    elif [ "$profile_changed" = "1" ]; then
        "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
        install -m 0600 "$proof_root/profile.before" "$profile" || true
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
        "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
        printf 'gaudere first real wake phase A: pre-acceptance failure restored wake-OFF profile\n' >&2
    fi
    exit "$status"
}
trap recover EXIT HUP INT TERM

snapshot_nonwake()
{
    output=$1
    expected_wakes=$2
    python3 - "$state_database" "$source_task" "$expected_reason" "$expected_delay_seconds" "$expected_wakes" > "$output" <<'PY'
import base64, json, pathlib, sqlite3, sys
path, task_id, reason, delay, expected_wakes = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
uri = pathlib.Path(path).resolve().as_uri() + "?mode=ro"
def enc(v): return {"bytes_base64": base64.b64encode(v).decode("ascii")} if isinstance(v, bytes) else v
def qi(v): return '"' + v.replace('"','""') + '"'
with sqlite3.connect(uri, uri=True) as db:
    db.execute("PRAGMA query_only=ON")
    if db.execute("PRAGMA user_version").fetchone()[0] != 4: raise SystemExit("schema is not 4")
    if [r[0] for r in db.execute("PRAGMA integrity_check")] != ["ok"]: raise SystemExit("integrity_check failed")
    if db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0] != expected_wakes: raise SystemExit("unexpected wake row count")
    blocker = db.execute("SELECT id,status FROM tasks WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1").fetchone()
    if blocker: raise SystemExit(f"nonterminal Task: {blocker[0]}:{blocker[1]}")
    blocker = db.execute("SELECT id,status FROM actions WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1").fetchone()
    if blocker: raise SystemExit(f"nonterminal Action: {blocker[0]}:{blocker[1]}")
    if db.execute("SELECT COUNT(*) FROM budget_consumptions WHERE scope=?", ("provider.call:openai.responses",)).fetchone()[0] != 4:
        raise SystemExit("provider consumption row count is not 4")
    row = db.execute("SELECT id,kind,status,result_content_type,result_output FROM tasks WHERE id=?", (task_id,)).fetchone()
    if not row or row[0] != task_id or row[1] != "cognition.reflect.v1" or row[2] != 3 or row[3] != "application/vnd.gaudere.cognition-decision+json":
        raise SystemExit("source Task durable fields mismatch")
    expected = {"schema":"gaudere.cognition.decision.v1","decision":"propose_wake","reason":reason,"wake_after_seconds":delay}
    if json.loads(row[4]) != expected: raise SystemExit("source Task durable decision mismatch")
    tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' AND name!='wake_intents' ORDER BY name")]
    contents = {}
    for table in tables:
        cols = db.execute(f"PRAGMA table_xinfo({qi(table)})").fetchall()
        rows = [[enc(v) for v in r] for r in db.execute(f"SELECT * FROM {qi(table)}").fetchall()]
        rows.sort(key=lambda r: json.dumps(r, ensure_ascii=False, sort_keys=True, separators=(",",":")))
        contents[table] = {"columns": cols, "rows": rows}
    objects = db.execute("SELECT type,name,tbl_name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND tbl_name!='wake_intents' ORDER BY type,name").fetchall()
print(json.dumps({"schema":4,"objects":objects,"tables":contents}, ensure_ascii=False, sort_keys=True, separators=(",",":")))
PY
}

phase=offline-baseline
"$systemctl_command" --user stop "$service_name"
[ "$(service_state)" = "inactive" ] || fail "service did not stop"
exec 9>>"$state_database.lock"
chmod 600 "$state_database.lock" 2>/dev/null || true
flock -n 9 || fail "state database is still owned after service stop"
snapshot_nonwake "$proof_root/nonwake-before.json" 0
sha256sum "$proof_root/nonwake-before.json" > "$proof_root/nonwake-before.sha256"
flock -u 9
exec 9>&-

phase=enable-profile
python3 - "$proof_root/profile.before" "$proof_root/profile.wake-enabled" <<'PY'
import pathlib, sys
src, dst = map(pathlib.Path, sys.argv[1:])
lines = src.read_text(encoding="utf-8").splitlines(keepends=True)
idx = [i for i,l in enumerate(lines) if l.startswith("Exec=")]
if len(idx) != 1: raise SystemExit("profile must contain exactly one Exec line")
i = idx[0]
if "--wake-intents" in lines[i]: raise SystemExit("profile already contains wake flag")
ending = "\n" if lines[i].endswith("\n") else ""
lines[i] = lines[i].rstrip("\n") + " --wake-intents" + ending
dst.write_text("".join(lines), encoding="utf-8")
PY
install -m 0600 "$proof_root/profile.wake-enabled" "$profile"
profile_changed=1
"$systemctl_command" --user daemon-reload
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "wake-enabled service did not start"
[ "$(running_image)" = "$frozen_runtime_image" ] || fail "running image changed while enabling WakeIntent"

phase=empty-status
empty_status=$(sh "$control_script" wake-status) || fail "wake-status failed after enabling capability"
printf '%s\n' "$empty_status"
printf '%s\n' "$empty_status" | grep -qx 'record=none' || fail "wake scope is not empty before acceptance"
printf '%s\n' "$empty_status" | grep -qx 'health=empty' || fail "empty wake scope is not healthy"

phase=accept
acceptance=$(sh "$control_script" accept-wake "$source_task") || fail "first wake acceptance failed"
wake_accepted=1
printf '%s\n' "$acceptance" | tee "$proof_root/acceptance.report"
[ "$(report_value acceptance "$acceptance")" = "accepted" ] || fail "first acceptance was not accepted"
[ "$(report_value status "$acceptance")" = "scheduled" ] || fail "accepted wake is not scheduled"
accepted_at=$(report_value accepted_at_ms "$acceptance")
due_at=$(report_value due_at_ms "$acceptance")
case "$accepted_at" in ''|*[!0-9]*) fail "invalid accepted_at_ms" ;; esac
case "$due_at" in ''|*[!0-9]*) fail "invalid due_at_ms" ;; esac
[ $((due_at - accepted_at)) -eq "$expected_delay_ms" ] || fail "durable deadline does not match 3600-second source delay"

phase=duplicate-proof
duplicate=$(sh "$control_script" accept-wake "$source_task") || fail "duplicate acceptance inspection failed"
printf '%s\n' "$duplicate" | tee "$proof_root/duplicate.report"
[ "$(report_value acceptance "$duplicate")" = "duplicate" ] || fail "second same-source acceptance was not duplicate"
[ "$(report_value accepted_at_ms "$duplicate")" = "$accepted_at" ] || fail "duplicate changed accepted_at"
[ "$(report_value due_at_ms "$duplicate")" = "$due_at" ] || fail "duplicate changed due_at"

phase=armed-status
status_before_restart=$(sh "$control_script" wake-status) || fail "wake-status unhealthy after acceptance"
printf '%s\n' "$status_before_restart" | tee "$proof_root/status-before-restart.report"
[ "$(report_value status "$status_before_restart")" = "scheduled" ] || fail "wake-status is not scheduled"
[ "$(report_value accepted_at_ms "$status_before_restart")" = "$accepted_at" ] || fail "wake-status accepted_at mismatch"
[ "$(report_value due_at_ms "$status_before_restart")" = "$due_at" ] || fail "wake-status due_at mismatch"
[ "$(report_value health "$status_before_restart")" = "ok" ] || fail "wake-status health is not ok"

now_ms=$(python3 - <<'PY'
import time
print(time.time_ns() // 1_000_000)
PY
)
[ "$now_ms" -lt "$due_at" ] || fail "deadline became due before restart proof"

phase=restart-proof
"$systemctl_command" --user restart "$service_name"
[ "$(service_state)" = "active" ] || fail "service not active after controlled restart"
status_after_restart=$(sh "$control_script" wake-status) || fail "wake-status unhealthy after restart"
printf '%s\n' "$status_after_restart" | tee "$proof_root/status-after-restart.report"
[ "$(report_value status "$status_after_restart")" = "scheduled" ] || fail "wake did not remain scheduled across restart"
[ "$(report_value accepted_at_ms "$status_after_restart")" = "$accepted_at" ] || fail "restart changed accepted_at"
[ "$(report_value due_at_ms "$status_after_restart")" = "$due_at" ] || fail "restart changed due_at"
[ "$(report_value health "$status_after_restart")" = "ok" ] || fail "restart re-arm health is not ok"

phase=nonwake-invariance
"$systemctl_command" --user stop "$service_name"
[ "$(service_state)" = "inactive" ] || fail "service did not stop for invariant snapshot"
exec 9>>"$state_database.lock"
flock -n 9 || fail "state database is owned during invariant snapshot"
snapshot_nonwake "$proof_root/nonwake-after-accept.json" 1
flock -u 9
exec 9>&-
cmp -s "$proof_root/nonwake-before.json" "$proof_root/nonwake-after-accept.json" \
    || fail "non-wake durable state changed during acceptance/restart proof"
"$systemctl_command" --user start "$service_name"
[ "$(service_state)" = "active" ] || fail "service not active after invariant snapshot"

phase=final-proof
final_status=$(sh "$control_script" wake-status) || fail "final scheduled wake-status unhealthy"
printf '%s\n' "$final_status" | tee "$proof_root/status-final-phase-a.report"
[ "$(report_value status "$final_status")" = "scheduled" ] || fail "wake is not scheduled at Phase A completion"
[ "$(report_value due_at_ms "$final_status")" = "$due_at" ] || fail "final due_at mismatch"
final_budget=$(sh "$control_script" budget)
printf '%s\n' "$final_budget" | tee "$proof_root/budget-phase-a-final.report"
[ "$(report_value total_used "$final_budget")" = "4" ] || fail "provider budget changed during first-wake Phase A"

profile_before_sha=$(sha256sum "$proof_root/profile.before" | awk '{print $1}')
profile_wake_sha=$(sha256sum "$proof_root/profile.wake-enabled" | awk '{print $1}')
cat > "$proof_root/phase-a.meta" <<EOF
source_task=$source_task
runtime_image=$frozen_runtime_image
agent_ref=$frozen_agent_ref
core_ref=$frozen_core_ref
wake_after_seconds=$expected_delay_seconds
accepted_at_ms=$accepted_at
due_at_ms=$due_at
profile_before_sha256=$profile_before_sha
profile_wake_sha256=$profile_wake_sha
provider_total=4
phase_a=PASS
EOF
chmod 0600 "$proof_root/phase-a.meta"

success=1
trap - EXIT HUP INT TERM
printf 'source_task=%s\n' "$source_task"
printf 'accepted_at_ms=%s\n' "$accepted_at"
printf 'due_at_ms=%s\n' "$due_at"
printf 'deadline_delta_ms=%s\n' "$expected_delay_ms"
printf 'duplicate_deadline_identity=PASS\n'
printf 'restart_rearm=PASS\n'
printf 'nonwake_state_unchanged=PASS\n'
printf 'provider_effects=0\n'
printf 'wake_acceptance_effects=1\n'
printf 'wake_status=scheduled\n'
printf 'wake_capability_active=true\n'
printf 'service_final=active\n'
printf 'gaudere first real wake phase A: PASS\n'
