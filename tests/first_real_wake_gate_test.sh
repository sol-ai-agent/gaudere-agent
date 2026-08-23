#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
phase_a="$repo_root/scripts/run-first-real-wake-phase-a-v0.sh"
phase_b="$repo_root/scripts/run-first-real-wake-phase-b-v0.sh"
workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-first-wake-test.XXXXXX")
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

fail()
{
    printf 'first_real_wake_gate_test: %s\n' "$*" >&2
    exit 1
}

sh -n "$phase_a"
sh -n "$phase_b"

# Static authority fences: neither phase may submit provider work; only Phase A
# contains the two intentional same-source accept-wake calls (first + duplicate).
! grep -E 'control_script" (reflect|openai) ' "$phase_a" "$phase_b" >/dev/null \
    || fail "first-wake gate contains a provider submission path"
[ "$(grep -c 'control_script" accept-wake' "$phase_a")" -eq 2 ] \
    || fail "Phase A must contain exactly first acceptance plus duplicate proof"
! grep -q 'accept-wake' "$phase_b" || fail "Phase B must not accept another wake"
grep -q 'too early; deadline is still' "$phase_b" || fail "Phase B lacks harmless early refusal"
grep -q 'wake slot may be consumed; service left stopped for manual review' "$phase_a" \
    || fail "Phase A lacks post-acceptance fail-closed recovery"

config="$workspace/config"
data="$workspace/data"
state_dir="$data/gaudere/state"
profile_dir="$config/containers/systemd"
mkdir -p "$state_dir" "$profile_dir"
state_db="$state_dir/state.db"
service_state_file="$workspace/service-state"
printf 'active\n' > "$service_state_file"
log="$workspace/control.log"
: > "$log"

reason='Resume after a one-hour production observation window to verify that the active pre-wake runtime leaves durable, interpretable evidence, journal the result, and identify the single reliability condition that should gate any future WakeIntent enablement. This advances cooperation reliability without spending scarce provider budget on a more ambitious step.'
python3 - "$state_db" "$reason" <<'PY'
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
PY

cat > "$profile_dir/gaudere-agent.container" <<'EOF'
[Container]
Image=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
Exec=--state /var/lib/gaudere/state.db --control-socket /tmp/gaudere-control.sock --openai-model gpt-5.6-sol --openai-secret gaudere-openai-api-key
EOF

fake_systemctl="$workspace/systemctl"
cat > "$fake_systemctl" <<'EOF'
#!/bin/sh
set -eu
state=${FAKE_SERVICE_STATE:?}
[ "$1" = "--user" ] || exit 9
op=$2
case "$op" in
  is-active) cat "$state" ;;
  stop) printf 'inactive\n' > "$state" ;;
  start|restart) printf 'active\n' > "$state" ;;
  daemon-reload) : ;;
  *) exit 9 ;;
esac
EOF
chmod +x "$fake_systemctl"

fake_podman="$workspace/podman"
cat > "$fake_podman" <<'EOF'
#!/bin/sh
set -eu
db=${FAKE_DB:?}
profile=${FAKE_PROFILE:?}
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
    shift 5
    cmd=$1
    shift
    printf '%s\n' "$cmd" >> "$log"
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
next_new_call=cooldown
BUDGET
        ;;
      task)
        python3 - "$db" "$1" <<'PY'
import json, sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    row=db.execute("SELECT id,kind,status,result_content_type,result_output FROM tasks WHERE id=?",(sys.argv[2],)).fetchone()
if not row:
    print("gaudere-agent: task not found", file=sys.stderr); raise SystemExit(3)
print("id="+json.dumps(row[0])); print("kind="+json.dumps(row[1])); print("status=succeeded")
print("attempts=1/2"); print("result_content_type="+json.dumps(row[3])); print("result_output="+json.dumps(row[4]))
print('result_metadata_content_type="application/vnd.gaudere.provider-usage+json"')
print('result_metadata="{}"')
PY
        ;;
      wake-status)
        if ! grep -q -- '--wake-intents' "$profile"; then
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
print('record=one'); print('id='+json.dumps(id)); print('source_task_id='+json.dumps(source))
print('source_consistency=eligible'); print('status='+('scheduled' if status==0 else 'fired'))
print(f'accepted_at_ms={accepted}'); print(f'due_at_ms={due}'); print('terminal_at_ms='+('none' if terminal is None else str(terminal)))
print('terminal_reason=""')
if status==0:
    print(f'derived_wake_at_ms={due}'); print('derived_lease_at_ms=none'); print(f'scheduler_next_at_ms={due}')
    print('health=ok'); print('scheduler_coverage=exact')
else:
    print('derived_wake_at_ms=none'); print('derived_lease_at_ms=none'); print('scheduler_next_at_ms=none')
    print('health=terminal'); print('scheduler_coverage=not_applicable')
PY
        ;;
      accept-wake)
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
      *) exit 9 ;;
    esac
    ;;
  *) exit 9 ;;
esac
EOF
chmod +x "$fake_podman"

run_a()
{
    XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$data" \
    FAKE_SERVICE_STATE="$service_state_file" FAKE_DB="$state_db" \
    FAKE_PROFILE="$profile_dir/gaudere-agent.container" FAKE_CONTROL_LOG="$log" \
    SYSTEMCTL="$fake_systemctl" PODMAN="$fake_podman" \
      sh "$phase_a" --execute-after-explicit-first-wake-go
}
run_b()
{
    XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$data" \
    FAKE_SERVICE_STATE="$service_state_file" FAKE_DB="$state_db" \
    FAKE_PROFILE="$profile_dir/gaudere-agent.container" FAKE_CONTROL_LOG="$log" \
    SYSTEMCTL="$fake_systemctl" PODMAN="$fake_podman" \
      sh "$phase_b" --observe-after-due-and-close
}

if XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$data" SYSTEMCTL="$fake_systemctl" PODMAN="$fake_podman" \
   FAKE_SERVICE_STATE="$service_state_file" FAKE_DB="$state_db" FAKE_PROFILE="$profile_dir/gaudere-agent.container" FAKE_CONTROL_LOG="$log" \
   sh "$phase_a" >"$workspace/noauth.out" 2>&1; then
    fail "Phase A accepted execution without explicit authorization"
fi

run_a > "$workspace/phase-a.out"
grep -qx 'deadline_delta_ms=3600000' "$workspace/phase-a.out" || fail "Phase A deadline proof missing"
grep -qx 'duplicate_deadline_identity=PASS' "$workspace/phase-a.out" || fail "duplicate proof missing"
grep -qx 'restart_rearm=PASS' "$workspace/phase-a.out" || fail "restart re-arm proof missing"
grep -qx 'nonwake_state_unchanged=PASS' "$workspace/phase-a.out" || fail "Phase A non-wake invariant missing"
grep -qx 'gaudere first real wake phase A: PASS' "$workspace/phase-a.out" || fail "Phase A did not PASS"
[ "$(grep -c '^accept-wake$' "$log")" -eq 2 ] || fail "synthetic Phase A did not execute exactly accepted+duplicate"
! grep -Eq '^(reflect|openai)$' "$log" || fail "synthetic Phase A invoked provider work"

if run_b > "$workspace/early.out" 2>&1; then
    fail "Phase B accepted observation before due"
fi
grep -q 'too early; deadline is still' "$workspace/early.out" || fail "Phase B early refusal reason missing"
grep -q -- '--wake-intents' "$profile_dir/gaudere-agent.container" || fail "early Phase B changed the profile"
[ "$(cat "$service_state_file")" = "active" ] || fail "early Phase B changed service state"

# Move only the synthetic wake clock evidence into the past and mark the inert
# terminal transition. Non-wake tables remain byte-for-byte unchanged.
python3 - "$state_db" "$data/gaudere/wake-proof-v0/first-real-wake/phase-a.meta" <<'PY'
import pathlib, sqlite3, sys, time
path, meta_path=sys.argv[1:]
accepted=time.time_ns()//1_000_000 - 7_200_000
due=accepted+3_600_000
terminal=due+1234
with sqlite3.connect(path) as db:
    db.execute("UPDATE wake_intents SET accepted_at_ms=?,due_at_ms=?,status=1,terminal_at_ms=?",(accepted,due,terminal))
meta=pathlib.Path(meta_path)
lines=[]
for line in meta.read_text().splitlines():
    if line.startswith('accepted_at_ms='): line=f'accepted_at_ms={accepted}'
    elif line.startswith('due_at_ms='): line=f'due_at_ms={due}'
    lines.append(line)
meta.write_text('\n'.join(lines)+'\n')
PY

run_b > "$workspace/phase-b.out"
grep -qx 'wake_terminal=fired' "$workspace/phase-b.out" || fail "Phase B fired proof missing"
grep -qx 'nonwake_state_unchanged=PASS' "$workspace/phase-b.out" || fail "Phase B non-wake invariant missing"
grep -qx 'wake_capability_active=false' "$workspace/phase-b.out" || fail "Phase B did not restore wake-OFF"
grep -qx 'provider_total_after=4' "$workspace/phase-b.out" || fail "Phase B provider invariant missing"
grep -qx 'gaudere first real wake phase B: PASS' "$workspace/phase-b.out" || fail "Phase B did not PASS"
! grep -q -- '--wake-intents' "$profile_dir/gaudere-agent.container" || fail "Phase B left WakeIntent enabled"
[ "$(cat "$service_state_file")" = "active" ] || fail "Phase B did not leave service active"
! grep -Eq '^(reflect|openai)$' "$log" || fail "first-wake lifecycle invoked provider work"

printf 'first_real_wake_gate_test: PASS\n'
