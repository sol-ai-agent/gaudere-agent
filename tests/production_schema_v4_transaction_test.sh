#!/bin/sh
set -eu

transaction=scripts/transition-production-schema-v4-wake-off.sh
candidate_id=sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
rollback_id=sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
agent_ref=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
core_ref=dddddddddddddddddddddddddddddddddddddddd
workspace=$(mktemp -d)
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

cat > "$workspace/systemctl" <<'SH'
#!/bin/sh
set -eu
state=${GAUDERE_FAKE_SERVICE_STATE:?}
running=${GAUDERE_FAKE_RUNNING_IMAGE:?}
profile=${GAUDERE_FAKE_PROFILE:?}
[ "$1" = "--user" ] || exit 90
case "$2" in
    is-active)
        value=$(cat "$state")
        printf '%s\n' "$value"
        [ "$value" = active ] && exit 0
        exit 3
        ;;
    is-enabled)
        if [ ! -f "$profile" ]; then
            printf 'not-found\n'
            exit 4
        fi
        if grep -q '^WantedBy=default.target$' "$profile"; then
            printf 'enabled\n'
            exit 0
        fi
        printf 'disabled\n'
        exit 1
        ;;
    stop)
        printf 'inactive\n' > "$state"
        exit 0
        ;;
    start)
        image=$(sed -n 's/^Image=//p' "$profile")
        [ -n "$image" ] || exit 91
        printf '%s\n' "$image" > "$running"
        printf 'active\n' > "$state"
        exit 0
        ;;
    daemon-reload)
        exit 0
        ;;
    reboot)
        if [ -f "$profile" ] && grep -q '^WantedBy=default.target$' "$profile"; then
            image=$(sed -n 's/^Image=//p' "$profile")
            [ -n "$image" ] || exit 91
            printf '%s\n' "$image" > "$running"
            printf 'active\n' > "$state"
        else
            printf 'inactive\n' > "$state"
        fi
        exit 0
        ;;
    *) exit 92 ;;
esac
SH
chmod +x "$workspace/systemctl"

cat > "$workspace/podman" <<'SH'
#!/bin/sh
set -eu
candidate=${GAUDERE_FAKE_CANDIDATE_ID:?}
rollback=${GAUDERE_FAKE_ROLLBACK_ID:?}
running=${GAUDERE_FAKE_RUNNING_IMAGE:?}
if [ "$1" = image ] && [ "$2" = exists ]; then
    exit 0
fi
if [ "$1" = image ] && [ "$2" = inspect ]; then
    ref=${5:-${4:-${3:-}}}
    case "$ref" in
        candidate|"$candidate") printf '%s\n' "$candidate" ;;
        rollback|"$rollback") printf '%s\n' "$rollback" ;;
        *) exit 93 ;;
    esac
    exit 0
fi
if [ "$1" = container ] && [ "$2" = inspect ]; then
    cat "$running"
    exit 0
fi
exit 94
SH
chmod +x "$workspace/podman"

cat > "$workspace/control.sh" <<'SH'
#!/bin/sh
set -eu
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
in_window_used=2
remaining_window=2
min_interval_seconds=900
last_consumed_at_ms=1787397758294
next_new_call=available
OUT
        ;;
    task)
        [ "$2" = production-initiative-first ] || exit 95
        cat <<'OUT'
id="production-initiative-first"
kind="provider.openai.responses"
status=succeeded
attempts=1/2
result_content_type="text/plain; charset=utf-8"
result_output="Gaudere"
result_metadata_content_type="application/vnd.gaudere.provider-usage+json"
result_metadata="{\"schema\":\"gaudere.provider_usage.v1\",\"provider\":\"openai\",\"model\":\"gpt-5.6-sol\",\"input_tokens\":166,\"output_tokens\":314,\"total_tokens\":480}"
OUT
        ;;
    wake)
        printf 'gaudere-agent: explicit wake capability is not enabled in this service\n' >&2
        exit 4
        ;;
    *) exit 96 ;;
esac
SH
chmod +x "$workspace/control.sh"

cat > "$workspace/deploy.sh" <<'SH'
#!/bin/sh
set -eu
state=${GAUDERE_STATE_DIR:?}
backups=${GAUDERE_BACKUP_DIR:?}
mode=${GAUDERE_FAKE_DEPLOY_MODE:-success}
parent=$(dirname "$state")
rollback="$parent/state.pre-v4-test"
failed="$parent/state.failed-v4-test"
archive="$backups/gaudere-state-test.tar.gz"
mkdir -p "$backups"
printf 'synthetic backup\n' > "$archive"

if [ "$mode" = fail-pre ]; then
    printf 'synthetic pre-swap failure\n' >&2
    exit 20
fi

mv "$state" "$rollback"
if [ "$mode" = power-loss-rename-gap ]; then
    transaction_pid=${GAUDERE_TEST_TRANSACTION_PID:?}
    kill -KILL "$transaction_pid"
    exit 137
fi
mkdir -p "$state"
cp "$rollback/state.db" "$state/state.db"
python3 - "$state/state.db" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    db.execute("PRAGMA user_version=4")
    db.execute("CREATE TABLE wake_intents(scope TEXT,id TEXT)")
PY

if [ "$mode" = fail-after-rollback ]; then
    mv "$state" "$failed"
    mv "$rollback" "$state"
    printf 'automatic_rollback=STARTED\n'
    printf 'automatic_rollback=PASS\n'
    exit 21
fi

printf 'backup=%s\n' "$archive"
printf 'rollback_directory=%s\n' "$rollback"
printf 'failed_state_directory_if_needed=%s\n' "$failed"
printf 'service_state=inactive\n'
printf 'wake_capability_active=false\n'
printf 'gaudere staged schema v4 deployment: PREPARED\n'
SH
chmod +x "$workspace/deploy.sh"

cat > "$workspace/wake-gate.sh" <<'SH'
#!/bin/sh
set -eu
state=${GAUDERE_STATE_DIR:?}
mode=${GAUDERE_FAKE_WAKE_MODE:-success}
profile=${GAUDERE_FAKE_PROFILE:?}
service_state=${GAUDERE_FAKE_SERVICE_STATE:?}
running=${GAUDERE_FAKE_RUNNING_IMAGE:?}
candidate=${GAUDERE_EXPECTED_RUNTIME_IMAGE_ID:?}
autostart=${GAUDERE_QUADLET_AUTOSTART:?}
[ "$autostart" = disarmed ] || exit 31

python3 - "$state/state.db" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 4
PY

python3 - deploy/quadlet/gaudere-agent-openai.container.in "$profile" "$candidate" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
image = sys.argv[3]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
indexes = [i for i, line in enumerate(lines) if line.startswith("Image=")]
assert len(indexes) == 1
ending = "\n" if lines[indexes[0]].endswith("\n") else ""
lines[indexes[0]] = f"Image={image}{ending}"
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
target.write_text("".join(filtered), encoding="utf-8")
PY
chmod 600 "$profile"
if [ "$mode" = fail ]; then
    printf 'inactive\n' > "$service_state"
    printf 'synthetic wake-off gate failure\n' >&2
    exit 30
fi
printf '%s\n' "$candidate" > "$running"
printf 'active\n' > "$service_state"
printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'service_final=active\n'
printf 'runtime_image_identity=PASS\n'
printf 'profile_autostart=disarmed\n'
printf 'gaudere schema v4 wake-off service gate: PASS\n'
SH
chmod +x "$workspace/wake-gate.sh"

make_fixture()
{
    root=$1
    autostart=${2:-enabled}
    state="$root/data/gaudere/state"
    config="$root/config/containers/systemd"
    mkdir -p "$state" "$config" "$root/backups"
    python3 - "$state/state.db" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    db.executescript("""
    PRAGMA user_version=3;
    CREATE TABLE tasks (id TEXT PRIMARY KEY,status INTEGER,result_metadata_content_type TEXT,result_metadata TEXT);
    INSERT INTO tasks VALUES(
      'production-initiative-first',3,
      'application/vnd.gaudere.provider-usage+json','{"total_tokens":480}'
    );
    CREATE TABLE actions (id TEXT PRIMARY KEY,status INTEGER);
    CREATE TABLE budget_consumptions (
      scope TEXT NOT NULL,idempotency_key TEXT NOT NULL,consumed_at_ms INTEGER NOT NULL,
      PRIMARY KEY(scope,idempotency_key)
    );
    INSERT INTO budget_consumptions VALUES
      ('provider.call:openai.responses','one',1),
      ('provider.call:openai.responses','two',2),
      ('provider.call:openai.responses','three',3);
    """)
PY
    printf 'Image=%s\n' "$rollback_id" > "$config/gaudere-agent.container"
    if [ "$autostart" = enabled ]; then
        printf '\n[Install]\nWantedBy=default.target\n' >> \
            "$config/gaudere-agent.container"
    elif [ "$autostart" != disarmed ]; then
        printf 'invalid synthetic autostart mode: %s\n' "$autostart" >&2
        exit 1
    fi
    chmod 600 "$config/gaudere-agent.container"
    printf 'active\n' > "$root/service.state"
    printf '%s\n' "$rollback_id" > "$root/running.image"
}

schema()
{
    python3 - "$1" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    print(db.execute("PRAGMA user_version").fetchone()[0])
PY
}

fake_enablement()
{
    root=$1
    GAUDERE_FAKE_SERVICE_STATE="$root/service.state" \
    GAUDERE_FAKE_RUNNING_IMAGE="$root/running.image" \
    GAUDERE_FAKE_PROFILE="$root/config/containers/systemd/gaudere-agent.container" \
        "$workspace/systemctl" --user is-enabled gaudere-agent.service 2>/dev/null \
        || true
}

simulate_reboot()
{
    root=$1
    GAUDERE_FAKE_SERVICE_STATE="$root/service.state" \
    GAUDERE_FAKE_RUNNING_IMAGE="$root/running.image" \
    GAUDERE_FAKE_PROFILE="$root/config/containers/systemd/gaudere-agent.container" \
        "$workspace/systemctl" --user reboot gaudere-agent.service
}

run_case()
{
    root=$1
    deploy_mode=$2
    wake_mode=$3
    GAUDERE_TEST_MODE=1 \
    GAUDERE_STATE_DIR="$root/data/gaudere/state" \
    GAUDERE_BACKUP_DIR="$root/backups" \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    GAUDERE_CANDIDATE_IMAGE=candidate \
    GAUDERE_EXPECTED_AGENT_REF="$agent_ref" \
    GAUDERE_EXPECTED_CORE_REF="$core_ref" \
    GAUDERE_EXPECTED_CANDIDATE_ID="$candidate_id" \
    GAUDERE_ROLLBACK_IMAGE=rollback \
    GAUDERE_EXPECTED_ROLLBACK_ID="$rollback_id" \
    GAUDERE_SCHEMA_V4_DEPLOY_SCRIPT="$workspace/deploy.sh" \
    GAUDERE_SCHEMA_V4_WAKE_OFF_GATE="$workspace/wake-gate.sh" \
    GAUDERE_CONTROL_SCRIPT="$workspace/control.sh" \
    GAUDERE_FAKE_DEPLOY_MODE="$deploy_mode" \
    GAUDERE_FAKE_WAKE_MODE="$wake_mode" \
    GAUDERE_FAKE_SERVICE_STATE="$root/service.state" \
    GAUDERE_FAKE_RUNNING_IMAGE="$root/running.image" \
    GAUDERE_FAKE_PROFILE="$root/config/containers/systemd/gaudere-agent.container" \
    GAUDERE_FAKE_CANDIDATE_ID="$candidate_id" \
    GAUDERE_FAKE_ROLLBACK_ID="$rollback_id" \
    XDG_CONFIG_HOME="$root/config" \
    SYSTEMCTL="$workspace/systemctl" \
    PODMAN="$workspace/podman" \
    sh "$transaction" production-initiative-first
}

# Success: one transaction ends active on schema v4 with the immutable candidate,
# while provider/wake effects remain zero.
case_success="$workspace/success"
make_fixture "$case_success"
run_case "$case_success" success success > "$case_success/out" 2> "$case_success/err"
cat "$case_success/out"
[ "$(schema "$case_success/data/gaudere/state/state.db")" = 4 ]
[ "$(cat "$case_success/service.state")" = active ]
[ "$(cat "$case_success/running.image")" = "$candidate_id" ]
grep -qx "Image=$candidate_id" "$case_success/config/containers/systemd/gaudere-agent.container"
grep -qx 'WantedBy=default.target' \
    "$case_success/config/containers/systemd/gaudere-agent.container"
[ "$(fake_enablement "$case_success")" = enabled ]
grep -q '^provider_effects=0$' "$case_success/out"
grep -q '^wake_effects=0$' "$case_success/out"
grep -q '^service_final=active$' "$case_success/out"
grep -q '^profile_autostart_final=enabled$' "$case_success/out"
grep -q '^gaudere production schema v4 wake-off transaction: PASS$' "$case_success/out"
test -d "$case_success/data/gaudere/state.pre-v4-test"

# A service that was active but intentionally not configured for boot remains
# disarmed after success while the candidate process itself stays active.
case_success_disabled="$workspace/success-disabled"
make_fixture "$case_success_disabled" disarmed
run_case "$case_success_disabled" success success \
    > "$case_success_disabled/out" 2> "$case_success_disabled/err"
[ "$(schema "$case_success_disabled/data/gaudere/state/state.db")" = 4 ]
[ "$(cat "$case_success_disabled/service.state")" = active ]
[ "$(cat "$case_success_disabled/running.image")" = "$candidate_id" ]
[ "$(fake_enablement "$case_success_disabled")" = disabled ]
if grep -q '^WantedBy=' \
        "$case_success_disabled/config/containers/systemd/gaudere-agent.container"; then
    printf 'disabled success unexpectedly rearmed autostart\n' >&2
    exit 1
fi
grep -q '^profile_autostart_final=disarmed$' "$case_success_disabled/out"

# Failure before any swap: restore/reload the exact original profile and restart
# unchanged schema v3 automatically.
case_pre="$workspace/fail-pre"
make_fixture "$case_pre"
cp "$case_pre/config/containers/systemd/gaudere-agent.container" \
    "$case_pre/profile.expected"
if run_case "$case_pre" fail-pre success > "$case_pre/out" 2> "$case_pre/err"; then
    printf 'pre-swap failure unexpectedly succeeded\n' >&2
    exit 1
fi
cat "$case_pre/err"
[ "$(schema "$case_pre/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_pre/service.state")" = active ]
[ "$(cat "$case_pre/running.image")" = "$rollback_id" ]
grep -qx "Image=$rollback_id" "$case_pre/config/containers/systemd/gaudere-agent.container"
cmp "$case_pre/profile.expected" \
    "$case_pre/config/containers/systemd/gaudere-agent.container"
[ "$(fake_enablement "$case_pre")" = enabled ]
grep -q '^pre_swap_recovery=PASS$' "$case_pre/err"
grep -q '^service_restored=active$' "$case_pre/err"

case_pre_disabled="$workspace/fail-pre-disabled"
make_fixture "$case_pre_disabled" disarmed
cp "$case_pre_disabled/config/containers/systemd/gaudere-agent.container" \
    "$case_pre_disabled/profile.expected"
if run_case "$case_pre_disabled" fail-pre success \
        > "$case_pre_disabled/out" 2> "$case_pre_disabled/err"; then
    printf 'disabled pre-swap failure unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(schema "$case_pre_disabled/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_pre_disabled/service.state")" = active ]
[ "$(cat "$case_pre_disabled/running.image")" = "$rollback_id" ]
cmp "$case_pre_disabled/profile.expected" \
    "$case_pre_disabled/config/containers/systemd/gaudere-agent.container"
[ "$(fake_enablement "$case_pre_disabled")" = disabled ]
grep -q '^pre_swap_recovery=PASS$' "$case_pre_disabled/err"

# Failure after the state swap: put exact v3 back, retain failed v4, and leave both
# the service and Quadlet autostart durably disarmed for human review.
case_post="$workspace/fail-post"
make_fixture "$case_post"
if run_case "$case_post" success fail > "$case_post/out" 2> "$case_post/err"; then
    printf 'post-swap failure unexpectedly succeeded\n' >&2
    exit 1
fi
cat "$case_post/err"
[ "$(schema "$case_post/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_post/service.state")" = inactive ]
test ! -e "$case_post/config/containers/systemd/gaudere-agent.container"
[ "$(fake_enablement "$case_post")" = not-found ]
grep -q '^transaction_rollback=PASS$' "$case_post/err"
grep -q '^service_left=inactive$' "$case_post/err"
grep -q '^autostart_left=disarmed$' "$case_post/err"
test -d "$case_post/data/gaudere/state.failed-v4-test"
[ "$(schema "$case_post/data/gaudere/state.failed-v4-test/state.db")" = 4 ]
simulate_reboot "$case_post"
[ "$(cat "$case_post/service.state")" = inactive ]

# If the inner deployment crossed the swap and already restored v3 itself, the
# outer transaction must recognize the exact snapshot, not demand a consumed
# rollback directory, and still leave the service stopped.
case_inner="$workspace/fail-inner-rollback"
make_fixture "$case_inner"
if run_case "$case_inner" fail-after-rollback success > "$case_inner/out" 2> "$case_inner/err"; then
    printf 'inner rollback failure unexpectedly succeeded\n' >&2
    exit 1
fi
cat "$case_inner/err"
[ "$(schema "$case_inner/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_inner/service.state")" = inactive ]
test ! -e "$case_inner/config/containers/systemd/gaudere-agent.container"
[ "$(fake_enablement "$case_inner")" = not-found ]
grep -q '^transaction_rollback=PASS_ALREADY_RESTORED_BY_STAGE$' "$case_inner/err"

# A synthetic SIGKILL immediately after state -> state.pre-v4 proves that traps are
# not part of the safety boundary. The canonical profile is already absent, so a
# simulated reboot cannot start a unit or create a replacement bind-mount source.
case_power="$workspace/power-loss-rename-gap"
make_fixture "$case_power" enabled
if run_case "$case_power" power-loss-rename-gap success \
        > "$case_power/out" 2> "$case_power/err"; then
    printf 'rename-gap power loss unexpectedly succeeded\n' >&2
    exit 1
fi
test ! -e "$case_power/data/gaudere/state"
test -d "$case_power/data/gaudere/state.pre-v4-test"
[ "$(schema "$case_power/data/gaudere/state.pre-v4-test/state.db")" = 3 ]
test ! -e "$case_power/config/containers/systemd/gaudere-agent.container"
[ "$(fake_enablement "$case_power")" = not-found ]
[ "$(cat "$case_power/service.state")" = inactive ]
simulate_reboot "$case_power"
[ "$(cat "$case_power/service.state")" = inactive ]

power_workspace=$(find "$case_power/data/gaudere/.schema-v4-transitions" \
    -mindepth 1 -maxdepth 1 -type d -name 'transition.*' -print -quit)
[ -n "$power_workspace" ]
[ "$(cat "$power_workspace/phase")" = deploying-v4 ]
[ "$(cat "$power_workspace/autostart.fence")" = DISARMED ]
test -f "$power_workspace/profile.before"

# Exercise the documented operator order on the disposable layout: keep the
# profile absent, restore exact v3 first, then restore/reload/start rollback.
mv "$case_power/data/gaudere/state.pre-v4-test" \
    "$case_power/data/gaudere/state"
install -m 0600 "$power_workspace/profile.before" \
    "$case_power/config/containers/systemd/gaudere-agent.container"
GAUDERE_FAKE_SERVICE_STATE="$case_power/service.state" \
GAUDERE_FAKE_RUNNING_IMAGE="$case_power/running.image" \
GAUDERE_FAKE_PROFILE="$case_power/config/containers/systemd/gaudere-agent.container" \
    "$workspace/systemctl" --user daemon-reload
GAUDERE_FAKE_SERVICE_STATE="$case_power/service.state" \
GAUDERE_FAKE_RUNNING_IMAGE="$case_power/running.image" \
GAUDERE_FAKE_PROFILE="$case_power/config/containers/systemd/gaudere-agent.container" \
    "$workspace/systemctl" --user start gaudere-agent.service
[ "$(schema "$case_power/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_power/service.state")" = active ]
[ "$(cat "$case_power/running.image")" = "$rollback_id" ]
[ "$(fake_enablement "$case_power")" = enabled ]

printf 'gaudere production schema v4 transaction test: PASS\n'
