#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
arm="$repo_root/scripts/run-wake-host-downtime-arm-v0.sh"
poweroff="$repo_root/scripts/run-wake-host-downtime-poweroff-v0.sh"
observe="$repo_root/scripts/run-wake-host-downtime-observe-v0.sh"
workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-wake-host-downtime-test.XXXXXX")
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

fail()
{
    printf 'wake_host_downtime_gate_test: %s\n' "$*" >&2
    exit 1
}

sh -n "$arm"
sh -n "$poweroff"
sh -n "$observe"
! grep -Eq '(staging_control|production_control) (reflect|openai)' "$arm" "$poweroff" "$observe" \
    || fail "host-downtime proof contains provider submission"
[ "$(grep -c 'staging_control accept-wake' "$arm")" -eq 2 ] \
    || fail "arm phase must contain exactly accepted + duplicate wake calls"
! grep -q 'accept-wake' "$poweroff" || fail "poweroff phase must not accept a wake"
! grep -q 'accept-wake' "$observe" || fail "observe phase must not accept a wake"
! grep -q -- '--openai-' "$repo_root/scripts/run-wake-host-downtime-poweroff-v0.sh" \
    || fail "poweroff phase must not contain OpenAI capability"
! grep -q ' --user start "$staging_service"' "$observe" \
    || fail "observe phase must not manually start staging before autostart proof"
grep -q 'Network=none' "$arm" || fail "staging profile does not force Network=none"
grep -q 'WantedBy=default.target' "$arm" || fail "staging profile lacks reboot startup wiring"
grep -q 'boot_start_ms.*-gt.*due_at' "$observe" || fail "observe phase lacks boot-after-due proof"

config="$workspace/config"
data="$workspace/data"
prod_state="$data/gaudere/state"
profile_dir="$config/containers/systemd"
mkdir -p "$prod_state" "$profile_dir" "$workspace/services"
prod_db="$prod_state/state.db"
boot_id="$workspace/boot_id"
proc_stat="$workspace/proc_stat"
printf '11111111-1111-1111-1111-111111111111\n' > "$boot_id"
printf 'btime 1000\n' > "$proc_stat"
printf 'active\n' > "$workspace/services/gaudere-agent.service"
printf 'inactive\n' > "$workspace/services/gaudere-wake-staging.service"
control_log="$workspace/control.log"
systemctl_log="$workspace/systemctl.log"
: > "$control_log"
: > "$systemctl_log"

reason='Resume after a one-hour production observation window to verify that the active pre-wake runtime leaves durable, interpretable evidence, journal the result, and identify the single reliability condition that should gate any future WakeIntent enablement. This advances cooperation reliability without spending scarce provider budget on a more ambitious step.'
python3 - "$prod_db" "$reason" <<'PY'
import json, sqlite3, sys
path, reason = sys.argv[1:]
with sqlite3.connect(path) as db:
    db.executescript('''
        PRAGMA user_version=4;
        CREATE TABLE tasks(id TEXT PRIMARY KEY, kind TEXT, status INTEGER, result_content_type TEXT, result_output TEXT);
        CREATE TABLE actions(id TEXT PRIMARY KEY, status INTEGER);
        CREATE TABLE budget_consumptions(scope TEXT);
        CREATE TABLE wake_intents(
          scope TEXT NOT NULL, id TEXT NOT NULL, source_id TEXT NOT NULL,
          accepted_at_ms INTEGER NOT NULL, due_at_ms INTEGER NOT NULL,
          status INTEGER NOT NULL, terminal_at_ms INTEGER, terminal_reason TEXT NOT NULL,
          PRIMARY KEY(scope,id));
        CREATE TRIGGER wake_no_delete BEFORE DELETE ON wake_intents
        BEGIN SELECT RAISE(ABORT, 'wake rows are permanent'); END;
    ''')
    decision = json.dumps({
        "decision":"propose_wake",
        "reason":reason,
        "schema":"gaudere.cognition.decision.v1",
        "wake_after_seconds":3600,
    }, separators=(",",":"), sort_keys=True)
    db.execute("INSERT INTO tasks VALUES(?,?,?,?,?)", (
        "production-reflection-wake-source-first", "cognition.reflect.v1", 3,
        "application/vnd.gaudere.cognition-decision+json", decision))
    db.executemany("INSERT INTO budget_consumptions VALUES(?)", [("provider.call:openai.responses",)] * 4)
    db.execute("INSERT INTO wake_intents VALUES(?,?,?,?,?,?,?,?)", (
        "cognition.reflect.wake.v0", "production-reflection-wake-source-first",
        "production-reflection-wake-source-first", 1000, 3601000, 1, 3601000, ""))
PY

fake_systemctl="$workspace/systemctl"
cat > "$fake_systemctl" <<'EOF'
#!/bin/sh
set -eu
states=${FAKE_SERVICE_DIR:?}
log=${FAKE_SYSTEMCTL_LOG:?}
printf '%s\n' "$*" >> "$log"
[ "$1" = "--user" ] || exit 9
op=$2
case "$op" in
  is-active)
    unit=$3
    cat "$states/$unit" 2>/dev/null || printf 'inactive\n'
    ;;
  is-enabled)
    unit=$3
    [ "$unit" = "gaudere-wake-staging.service" ] || exit 9
    printf 'generated\n'
    ;;
  start|restart)
    unit=$3
    printf 'active\n' > "$states/$unit"
    ;;
  stop)
    unit=$3
    printf 'inactive\n' > "$states/$unit"
    ;;
  daemon-reload) : ;;
  *) exit 9 ;;
esac
EOF
chmod +x "$fake_systemctl"

fake_poweroff="$workspace/poweroff"
cat > "$fake_poweroff" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = "poweroff" ] || exit 9
printf 'requested\n' > "${FAKE_POWEROFF_MARKER:?}"
EOF
chmod +x "$fake_poweroff"

fake_podman="$workspace/podman"
cat > "$fake_podman" <<'EOF'
#!/bin/sh
set -eu
prod=${FAKE_PROD_DB:?}
stage=${FAKE_STAGE_DB:?}
log=${FAKE_CONTROL_LOG:?}
runtime=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
agent=4e6cb09467456f38377bd8610e1ac534c7705380
core=1316cf68db93e4c91a7bd79fbd289b8f382f8659
case "$1:$2" in
  container:inspect)
    printf '%s\n' "$runtime"
    ;;
  image:inspect)
    case "$4" in
      *agent.revision*) printf '%s\n' "$agent" ;;
      *core.revision*) printf '%s\n' "$core" ;;
      *) exit 9 ;;
    esac
    ;;
  exec:*)
    container=$2
    [ "$container" = "gaudere-agent" ] && db=$prod || db=$stage
    shift 5
    cmd=$1
    shift
    printf '%s:%s\n' "$container" "$cmd" >> "$log"
    case "$cmd" in
      budget)
        cat <<'BUDGET'
scope="provider.call:openai.responses"
provider_enabled=true
max_total=12
total_used=4
remaining_total=8
max_window=4
window_seconds=86400
in_window_used=1
remaining_window=3
min_interval_seconds=900
last_consumed_at_ms=123
next_new_call=available
BUDGET
        ;;
      task)
        python3 - "$db" "$1" <<'PY'
import json, sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    row=db.execute("SELECT id,kind,status,result_content_type,result_output FROM tasks WHERE id=?",(sys.argv[2],)).fetchone()
if not row: raise SystemExit(3)
print("id="+json.dumps(row[0])); print("kind="+json.dumps(row[1])); print("status=succeeded")
print("attempts=1/2"); print("result_content_type="+json.dumps(row[3])); print("result_output="+json.dumps(row[4]))
print('result_metadata_content_type="application/vnd.gaudere.provider-usage+json"'); print('result_metadata="{}"')
PY
        ;;
      wake-status)
        if [ "$container" = "gaudere-agent" ]; then
          printf 'gaudere-agent: explicit wake capability is not enabled in this service\n' >&2
          exit 4
        fi
        python3 - "$db" <<'PY'
import json, sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    row=db.execute("SELECT id,source_id,status,accepted_at_ms,due_at_ms,terminal_at_ms FROM wake_intents").fetchone()
print('report_schema="gaudere.wake_status.v1"'); print('scope="cognition.reflect.wake.v0"')
if not row:
    print('record=none'); print('health=empty'); print('scheduler_coverage=not_applicable'); raise SystemExit
id,source,status,accepted,due,terminal=row
print('record=one'); print('id='+json.dumps(id)); print('source_task_id='+json.dumps(source)); print('source_consistency=eligible')
print('status='+('scheduled' if status==0 else 'fired')); print(f'accepted_at_ms={accepted}'); print(f'due_at_ms={due}')
print('terminal_at_ms='+('none' if terminal is None else str(terminal))); print('terminal_reason=""')
if status==0:
    print(f'derived_wake_at_ms={due}'); print('derived_lease_at_ms=none'); print(f'scheduler_next_at_ms={due}')
    print('health=ok'); print('scheduler_coverage=exact')
else:
    print('derived_wake_at_ms=none'); print('derived_lease_at_ms=none'); print('scheduler_next_at_ms=none')
    print('health=terminal'); print('scheduler_coverage=not_applicable')
PY
        ;;
      accept-wake)
        [ "$container" = "gaudere-wake-staging" ] || exit 9
        python3 - "$db" "$1" <<'PY'
import sqlite3, sys, time
path, source=sys.argv[1:]
with sqlite3.connect(path) as db:
    row=db.execute("SELECT accepted_at_ms,due_at_ms,status FROM wake_intents WHERE id=?",(source,)).fetchone()
    if row:
        acceptance='duplicate'; accepted,due,status=row
    else:
        acceptance='accepted'; accepted=time.time_ns()//1_000_000; due=accepted+3_600_000; status=0
        db.execute("INSERT INTO wake_intents VALUES(?,?,?,?,?,?,?,?)",('cognition.reflect.wake.v0',source,source,accepted,due,0,None,''))
print('acceptance='+acceptance); print('scope="cognition.reflect.wake.v0"'); print('id="'+source+'"'); print('source_id="'+source+'"')
print('status=scheduled'); print(f'accepted_at_ms={accepted}'); print(f'due_at_ms={due}'); print('terminal_at_ms=none'); print('terminal_reason=""')
PY
        ;;
      reflect|openai) exit 88 ;;
      *) exit 9 ;;
    esac
    ;;
  *) exit 9 ;;
esac
EOF
chmod +x "$fake_podman"

common_env()
{
    env XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$data" \
        GAUDERE_BOOT_ID_FILE="$boot_id" GAUDERE_PROC_STAT_FILE="$proc_stat" \
        FAKE_SERVICE_DIR="$workspace/services" FAKE_SYSTEMCTL_LOG="$systemctl_log" \
        FAKE_PROD_DB="$prod_db" FAKE_STAGE_DB="$data/gaudere/wake-host-downtime-v0/state/state.db" \
        FAKE_CONTROL_LOG="$control_log" SYSTEMCTL="$fake_systemctl" PODMAN="$fake_podman" "$@"
}

if common_env sh "$arm" >"$workspace/noauth.out" 2>&1; then
    fail "arm phase accepted execution without explicit authorization"
fi
common_env sh "$arm" --prepare-after-explicit-host-downtime-go > "$workspace/arm.out"
grep -qx 'deadline_delta_ms=3600000' "$workspace/arm.out" || fail "arm deadline proof missing"
grep -qx 'duplicate_deadline_identity=PASS' "$workspace/arm.out" || fail "arm duplicate proof missing"
grep -qx 'staging_isolation=PASS' "$workspace/arm.out" || fail "staging isolation proof missing"
grep -qx 'staging_network=none' "$workspace/arm.out" || fail "Network=none proof missing"
grep -qx 'production_untouched=PASS' "$workspace/arm.out" || fail "production invariant missing"
grep -qx 'gaudere wake host-downtime arm: PASS' "$workspace/arm.out" || fail "arm phase did not PASS"
[ "$(grep -c '^gaudere-wake-staging:accept-wake$' "$control_log")" -eq 2 ] || fail "arm did not issue exactly two staged acceptance calls"
! grep -Eq ':(reflect|openai)$' "$control_log" || fail "arm invoked provider work"

stage_db="$data/gaudere/wake-host-downtime-v0/state/state.db"
python3 - "$stage_db" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    rows=db.execute("SELECT name FROM sqlite_master WHERE type='trigger' AND tbl_name='wake_intents'").fetchall()
    wakes=db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0]
if rows != [('wake_no_delete',)]: raise SystemExit(f"wake trigger was not restored: {rows!r}")
if wakes != 1: raise SystemExit("staging wake was not accepted exactly once")
PY

marker="$workspace/poweroff.marker"
if common_env GAUDERE_POWEROFF_COMMAND="$fake_poweroff" FAKE_POWEROFF_MARKER="$marker" \
   sh "$poweroff" >"$workspace/poweroff-noauth.out" 2>&1; then
    fail "poweroff phase accepted execution without explicit authorization"
fi
common_env GAUDERE_POWEROFF_COMMAND="$fake_poweroff" FAKE_POWEROFF_MARKER="$marker" \
    sh "$poweroff" --poweroff-after-explicit-host-downtime-go > "$workspace/poweroff.out"
[ -f "$marker" ] || fail "synthetic poweroff command was not invoked"
grep -qx 'status=POWER_OFF_REQUESTED_BEFORE_DUE' "$workspace/poweroff.out" || fail "poweroff witness status missing"

proof="$data/gaudere/wake-proof-v0/host-downtime"
due=$(sed -n 's/^due_at_ms=//p' "$proof/phase-arm.meta")
accepted=$(sed -n 's/^accepted_at_ms=//p' "$proof/phase-arm.meta")
terminal=$((due + 12345))
python3 - "$stage_db" "$terminal" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    db.execute("UPDATE wake_intents SET status=1,terminal_at_ms=?",(int(sys.argv[2]),))
PY
printf '22222222-2222-2222-2222-222222222222\n' > "$boot_id"
printf 'btime %s\n' $((due / 1000 + 10)) > "$proc_stat"
printf 'active\n' > "$workspace/services/gaudere-agent.service"
printf 'active\n' > "$workspace/services/gaudere-wake-staging.service"

if common_env sh "$observe" >"$workspace/observe-noauth.out" 2>&1; then
    fail "observe phase accepted execution without explicit argument"
fi
common_env sh "$observe" --observe-after-reboot-and-close > "$workspace/observe.out"
grep -qx 'host_down_across_deadline=PASS' "$workspace/observe.out" || fail "host-down proof missing"
grep -qx 'staging_autostart_after_reboot=PASS' "$workspace/observe.out" || fail "autostart proof missing"
grep -qx 'single_terminal_transition=PASS' "$workspace/observe.out" || fail "at-most-once proof missing"
grep -qx 'nonwake_state_unchanged=PASS' "$workspace/observe.out" || fail "non-wake invariant missing"
grep -qx 'provider_effects=0' "$workspace/observe.out" || fail "provider invariant missing"
grep -qx 'successor_effects=0' "$workspace/observe.out" || fail "successor invariant missing"
grep -qx 'production_untouched=PASS' "$workspace/observe.out" || fail "production final invariant missing"
grep -qx 'staging_profile_removed=PASS' "$workspace/observe.out" || fail "staging cleanup proof missing"
grep -qx 'gaudere wake host-downtime observe: PASS' "$workspace/observe.out" || fail "observe phase did not PASS"
[ ! -e "$profile_dir/gaudere-wake-staging.container" ] || fail "staging profile remained after PASS"
[ ! -e "$data/gaudere/wake-host-downtime-v0" ] || fail "staging state remained after PASS"
[ -f "$proof/staging-final.db" ] || fail "final staging proof DB missing"
! grep -Eq ':(reflect|openai)$' "$control_log" || fail "host-downtime lifecycle invoked provider work"

# The observe phase may restart staging only after it has first proved post-reboot
# active/autostart state; it must never contain a manual start operation.
! grep -q '^--user start gaudere-wake-staging.service$' "$systemctl_log" \
    || fail "observe or another phase manually started staging unexpectedly after reboot fixture"

printf 'wake_host_downtime_gate_test: PASS\n'
