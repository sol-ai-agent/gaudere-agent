#!/bin/sh
set -eu

gate=scripts/validate-schema-v4-service-wake-off.sh
source_profile=deploy/quadlet/gaudere-agent-openai.container.in
workspace=$(mktemp -d)
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

fakebin="$workspace/bin"
config_home="$workspace/config"
state_directory="$workspace/state"
state_file="$workspace/service.state"
control_log="$workspace/control.log"
service_log="$workspace/service.log"
target_profile="$config_home/containers/systemd/gaudere-agent.container"
old_profile="$workspace/profile.old"
expected_id=sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
mkdir -p "$fakebin" "$state_directory" "$(dirname "$target_profile")"
printf 'inactive\n' > "$state_file"
printf 'OLD PROFILE\n' > "$old_profile"

cat > "$fakebin/systemctl" <<'SH'
#!/bin/sh
set -eu
state=${GAUDERE_FAKE_SERVICE_STATE:?}
log=${GAUDERE_FAKE_SERVICE_LOG:?}
[ "$1" = "--user" ] || exit 90
operation=$2
case "$operation" in
    is-active)
        value=$(cat "$state")
        printf '%s\n' "$value"
        [ "$value" = "active" ] && exit 0
        exit 3
        ;;
    is-enabled)
        profile=${GAUDERE_FAKE_TARGET_PROFILE:?}
        if [ -f "$profile" ] && grep -q '^WantedBy=default.target$' "$profile"; then
            printf 'enabled\n'
            exit 0
        fi
        printf 'disabled\n'
        exit 1
        ;;
    start)
        printf 'active\n' > "$state"
        printf 'start\n' >> "$log"
        exit 0
        ;;
    stop)
        printf 'inactive\n' > "$state"
        printf 'stop\n' >> "$log"
        exit 0
        ;;
    daemon-reload)
        printf 'daemon-reload\n' >> "$log"
        exit 0
        ;;
    *) exit 91 ;;
esac
SH
chmod +x "$fakebin/systemctl"

cat > "$fakebin/journalctl" <<'SH'
#!/bin/sh
set -eu
printf 'gaudere-agent: OpenAI provider enabled model=gpt-5.6-sol secret=gaudere-openai-api-key\n'
printf 'gaudere-agent: control socket=/tmp/gaudere-control.sock\n'
printf 'gaudere-agent: running\n'
printf 'gaudere-agent: safe\n'
if [ "${GAUDERE_FAKE_WAKE_LOG:-0}" = "1" ]; then
    printf 'gaudere-agent: explicit wake enabled scope=cognition.reflect.wake.v0 max_total=1 automatic_successor=false\n'
fi
SH
chmod +x "$fakebin/journalctl"

cat > "$fakebin/podman" <<'SH'
#!/bin/sh
set -eu
if [ "$1" = "image" ] && [ "$2" = "inspect" ]; then
    printf '%s\n' "${GAUDERE_FAKE_IMAGE_ID:?}"
    exit 0
fi
if [ "$1" = "container" ] && [ "$2" = "inspect" ]; then
    printf '%s\n' "${GAUDERE_FAKE_RUNNING_IMAGE_ID:-${GAUDERE_FAKE_IMAGE_ID:?}}"
    exit 0
fi
exit 92
SH
chmod +x "$fakebin/podman"

cat > "$workspace/installer.sh" <<'SH'
#!/bin/sh
set -eu
mkdir -p "$(dirname "${GAUDERE_FAKE_TARGET_PROFILE:?}")"
python3 - "${GAUDERE_FAKE_SOURCE_PROFILE:?}" "${GAUDERE_FAKE_TARGET_PROFILE:?}" \
        "${GAUDERE_IMAGE:?}" "${GAUDERE_QUADLET_AUTOSTART:-enabled}" <<'PY'
import pathlib
import sys
source, target = map(pathlib.Path, sys.argv[1:3])
image = sys.argv[3]
autostart = sys.argv[4]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
indexes = [i for i, line in enumerate(lines) if line.startswith("Image=")]
assert len(indexes) == 1
index = indexes[0]
ending = "\n" if lines[index].endswith("\n") else ""
lines[index] = f"Image={image}{ending}"
if autostart == "disarmed":
    filtered = []
    in_install = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_install = stripped == "[Install]"
            if in_install:
                continue
        if not in_install:
            filtered.append(line)
    lines = filtered
target.write_text("".join(lines), encoding="utf-8")
PY
chmod 600 "${GAUDERE_FAKE_TARGET_PROFILE:?}"
printf 'synthetic installer: profile pinned to %s, service remains stopped\n' "$GAUDERE_IMAGE"
SH
chmod +x "$workspace/installer.sh"

cat > "$workspace/control.sh" <<'SH'
#!/bin/sh
set -eu
log=${GAUDERE_FAKE_CONTROL_LOG:?}
printf '%s\n' "$*" >> "$log"
case "$1" in
    budget)
        cat <<'OUT'
scope="provider.call:openai.responses"
provider_enabled=true
max_total=12
total_used=3
remaining_total=9
max_window=4
window_seconds=86400
in_window_used=3
remaining_window=1
min_interval_seconds=900
last_consumed_at_ms=1787430000000
next_new_call=cooldown
OUT
        ;;
    task)
        [ "$2" = "production-initiative-first" ] || exit 93
        cat <<'OUT'
id="production-initiative-first"
kind="cognition.reflect.v1"
status=succeeded
attempts=1/2
result_content_type="application/vnd.gaudere.reflection+json"
result_output="{\"decision\":\"stop\"}"
result_metadata_content_type="application/vnd.gaudere.provider-usage+json"
result_metadata="{\"schema\":\"gaudere.provider_usage.v1\",\"provider\":\"openai\",\"model\":\"gpt-5.6-sol\",\"input_tokens\":10,\"output_tokens\":5,\"total_tokens\":15}"
OUT
        ;;
    wake)
        if [ "${GAUDERE_FAKE_WAKE_ENABLED:-0}" = "1" ]; then
            printf 'id="%s"\nstatus=scheduled\n' "$2"
            exit 0
        fi
        printf 'gaudere-agent: explicit wake capability is not enabled in this service\n' >&2
        exit 4
        ;;
    *)
        printf 'unexpected mutating control operation: %s\n' "$1" >&2
        exit 94
        ;;
esac
SH
chmod +x "$workspace/control.sh"

write_fixture()
{
    rm -f "$state_directory/state.db" "$state_directory/state.db-wal" \
        "$state_directory/state.db-shm" "$state_directory/state.db.lock"
    python3 - "$state_directory/state.db" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as db:
    db.executescript("""
    PRAGMA user_version=4;
    CREATE TABLE tasks (
      id TEXT PRIMARY KEY,
      idempotency_key TEXT,
      kind TEXT,
      input_content_type TEXT,
      input TEXT,
      max_input_bytes INTEGER,
      max_output_bytes INTEGER,
      max_runtime_ms INTEGER,
      max_attempts INTEGER,
      attempts_started INTEGER,
      status INTEGER,
      lease_owner TEXT,
      lease_expires_at_ms INTEGER,
      cancel_reason TEXT,
      result_content_type TEXT,
      result_output TEXT,
      result_failure_code TEXT,
      result_failure_message TEXT,
      result_metadata_content_type TEXT,
      result_metadata TEXT
    );
    CREATE TABLE actions (
      id TEXT PRIMARY KEY,
      idempotency_key TEXT,
      critical INTEGER,
      status INTEGER,
      effect_result INTEGER,
      lease_owner TEXT,
      lease_expires_at_ms INTEGER
    );
    CREATE TABLE budget_consumptions (
      scope TEXT NOT NULL,
      idempotency_key TEXT NOT NULL,
      consumed_at_ms INTEGER NOT NULL,
      PRIMARY KEY(scope,idempotency_key)
    );
    CREATE TABLE wake_intents (
      scope TEXT NOT NULL,
      id TEXT NOT NULL,
      source_id TEXT NOT NULL,
      accepted_at_ms INTEGER NOT NULL,
      due_at_ms INTEGER NOT NULL,
      status INTEGER NOT NULL,
      terminal_at_ms INTEGER,
      terminal_reason TEXT NOT NULL DEFAULT '',
      PRIMARY KEY(scope,id)
    );
    CREATE INDEX idx_wake_intents_scope_status_due
      ON wake_intents(scope,status,due_at_ms,id);
    CREATE TRIGGER wake_intents_require_scheduled_insert
      BEFORE INSERT ON wake_intents BEGIN SELECT 1; END;
    CREATE TRIGGER wake_intents_single_transition
      BEFORE UPDATE ON wake_intents BEGIN SELECT 1; END;
    CREATE TRIGGER wake_intents_prevent_delete
      BEFORE DELETE ON wake_intents BEGIN SELECT 1; END;
    INSERT INTO tasks VALUES (
      'production-initiative-first','reflection:production-initiative-first',
      'cognition.reflect.v1','text/plain','objective',16384,65536,60000,2,1,3,
      NULL,NULL,'','application/vnd.gaudere.reflection+json',
      '{"decision":"stop"}','','',
      'application/vnd.gaudere.provider-usage+json',
      '{"schema":"gaudere.provider_usage.v1","provider":"openai","model":"gpt-5.6-sol","input_tokens":10,"output_tokens":5,"total_tokens":15}'
    );
    INSERT INTO budget_consumptions VALUES
      ('provider.call:openai.responses','call-1',1000),
      ('provider.call:openai.responses','call-2',2000),
      ('provider.call:openai.responses','call-3',3000);
    """)
PY
}

run_gate()
{
    XDG_CONFIG_HOME="$config_home" \
    GAUDERE_TEST_MODE=1 \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    GAUDERE_STATE_DIR="$state_directory" \
    GAUDERE_EXPECTED_RUNTIME_IMAGE_ID="$expected_id" \
    GAUDERE_QUADLET_AUTOSTART="${GAUDERE_QUADLET_AUTOSTART:-enabled}" \
    GAUDERE_OPENAI_INSTALLER="$workspace/installer.sh" \
    GAUDERE_CONTROL_SCRIPT="$workspace/control.sh" \
    GAUDERE_FAKE_SOURCE_PROFILE="$source_profile" \
    GAUDERE_FAKE_TARGET_PROFILE="$target_profile" \
    GAUDERE_FAKE_SERVICE_STATE="$state_file" \
    GAUDERE_FAKE_SERVICE_LOG="$service_log" \
    GAUDERE_FAKE_CONTROL_LOG="$control_log" \
    GAUDERE_FAKE_IMAGE_ID="$expected_id" \
    SYSTEMCTL="$fakebin/systemctl" \
    JOURNALCTL="$fakebin/journalctl" \
    PODMAN="$fakebin/podman" \
    sh "$gate" production-initiative-first
}

reset_case()
{
    write_fixture
    printf 'inactive\n' > "$state_file"
    : > "$control_log"
    : > "$service_log"
    cp "$old_profile" "$target_profile"
}

# Success: service is left active, profile is pinned to the reviewed immutable image,
# and only observational live-control commands were issued.
reset_case
run_gate > "$workspace/pass.out"
cat "$workspace/pass.out"
grep -q '^schema_before=4$' "$workspace/pass.out"
grep -q '^wake_rows_before=0$' "$workspace/pass.out"
grep -q '^provider_budget_rows_before=3$' "$workspace/pass.out"
grep -q "^profile_image_id=$expected_id$" "$workspace/pass.out"
grep -q '^profile_wake_flag=absent$' "$workspace/pass.out"
grep -q '^durable_state_identity=PASS$' "$workspace/pass.out"
grep -q '^provider_effects=0$' "$workspace/pass.out"
grep -q '^wake_effects=0$' "$workspace/pass.out"
grep -q '^service_final=active$' "$workspace/pass.out"
grep -q '^runtime_image_identity=PASS$' "$workspace/pass.out"
grep -q '^gaudere schema v4 wake-off service gate: PASS$' "$workspace/pass.out"
[ "$(cat "$state_file")" = "active" ]
grep -qx "Image=$expected_id" "$target_profile"
if grep -Eq '^(echo|openai|reflect|accept-wake|revoke-wake)( |$)' "$control_log"; then
    printf 'wake-off gate issued a mutating live-control command\n' >&2
    cat "$control_log" >&2
    exit 1
fi
[ "$(grep -c '^wake ' "$control_log")" -eq 2 ]
[ "$(grep -c '^budget$' "$control_log")" -eq 2 ]
[ "$(grep -c '^task production-initiative-first$' "$control_log")" -eq 2 ]

# The production transaction can start the same immutable candidate for live probes
# while omitting every [Install]/WantedBy directive. With no previous source, a
# failed disarmed gate must remove the candidate source again.
reset_case
rm -f -- "$target_profile"
GAUDERE_QUADLET_AUTOSTART=disarmed run_gate > "$workspace/disarmed.out"
grep -q '^profile_autostart=disarmed$' "$workspace/disarmed.out"
grep -qx "Image=$expected_id" "$target_profile"
if grep -q '^\[Install\]$\|^WantedBy=' "$target_profile"; then
    printf 'disarmed wake-off gate retained an automatic-start directive\n' >&2
    exit 1
fi
[ "$(cat "$state_file")" = active ]

reset_case
rm -f -- "$target_profile"
wrong_id=sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
if GAUDERE_QUADLET_AUTOSTART=disarmed GAUDERE_FAKE_RUNNING_IMAGE_ID="$wrong_id" \
        run_gate > "$workspace/disarmed-fail.out" 2> "$workspace/disarmed-fail.err"; then
    printf 'disarmed gate unexpectedly accepted a running image mismatch\n' >&2
    exit 1
fi
grep -q 'recovery_profile_restored=true' "$workspace/disarmed-fail.err"
test ! -e "$target_profile"
[ "$(cat "$state_file")" = inactive ]

# A pre-existing WakeIntent must stop before profile installation or service start.
reset_case
python3 - "$state_directory/state.db" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    db.execute(
        "INSERT INTO wake_intents(scope,id,source_id,accepted_at_ms,due_at_ms,status) "
        "VALUES(?,?,?,?,?,?)",
        ("cognition.reflect.wake.v0","existing","source",1,2,0),
    )
PY
if run_gate > "$workspace/wake-row.out" 2> "$workspace/wake-row.err"; then
    printf 'gate unexpectedly accepted a pre-existing WakeIntent\n' >&2
    exit 1
fi
grep -q 'wake_intents is not empty' "$workspace/wake-row.err"
cmp "$old_profile" "$target_profile"
[ "$(cat "$state_file")" = "inactive" ]
[ ! -s "$control_log" ]

# If the actual running container does not use the pinned candidate image, fail closed,
# restore the old profile, and leave the service stopped.
reset_case
if GAUDERE_FAKE_RUNNING_IMAGE_ID="$wrong_id" run_gate \
    > "$workspace/running-image.out" 2> "$workspace/running-image.err"; then
    printf 'gate unexpectedly accepted a running container image mismatch\n' >&2
    exit 1
fi
grep -q 'running container image drift' "$workspace/running-image.err"
grep -q 'recovery_profile_restored=true' "$workspace/running-image.err"
cmp "$old_profile" "$target_profile"
[ "$(cat "$state_file")" = "inactive" ]

# If live control says wake is enabled, the gate must restore the prior profile and
# leave the service stopped for review.
reset_case
if GAUDERE_FAKE_WAKE_ENABLED=1 run_gate \
    > "$workspace/wake-enabled.out" 2> "$workspace/wake-enabled.err"; then
    printf 'gate unexpectedly accepted enabled WakeIntent live control\n' >&2
    exit 1
fi
grep -q 'observational wake lookup unexpectedly succeeded' "$workspace/wake-enabled.err"
grep -q 'recovery_profile_restored=true' "$workspace/wake-enabled.err"
cmp "$old_profile" "$target_profile"
[ "$(cat "$state_file")" = "inactive" ]

# A wake-enabled readiness log is independently fatal even when the observational
# command remains disabled.
reset_case
if GAUDERE_FAKE_WAKE_LOG=1 run_gate \
    > "$workspace/wake-log.out" 2> "$workspace/wake-log.err"; then
    printf 'gate unexpectedly accepted explicit-wake-enabled service log\n' >&2
    exit 1
fi
grep -q 'service log reports explicit wake enabled' "$workspace/wake-log.err"
grep -q 'recovery_profile_restored=true' "$workspace/wake-log.err"
cmp "$old_profile" "$target_profile"
[ "$(cat "$state_file")" = "inactive" ]

printf 'gaudere schema v4 wake-off service gate test: PASS\n'
