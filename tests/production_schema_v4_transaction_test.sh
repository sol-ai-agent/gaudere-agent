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

python3 - "$state/state.db" <<'PY'
import sqlite3, sys
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 4
PY

printf 'Image=%s\n' "$candidate" > "$profile"
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
printf 'gaudere schema v4 wake-off service gate: PASS\n'
SH
chmod +x "$workspace/wake-gate.sh"

make_fixture()
{
    root=$1
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
grep -q '^provider_effects=0$' "$case_success/out"
grep -q '^wake_effects=0$' "$case_success/out"
grep -q '^service_final=active$' "$case_success/out"
grep -q '^gaudere production schema v4 wake-off transaction: PASS$' "$case_success/out"
test -d "$case_success/data/gaudere/state.pre-v4-test"

# Failure before any swap: restore/reload the exact original profile and restart
# unchanged schema v3 automatically.
case_pre="$workspace/fail-pre"
make_fixture "$case_pre"
if run_case "$case_pre" fail-pre success > "$case_pre/out" 2> "$case_pre/err"; then
    printf 'pre-swap failure unexpectedly succeeded\n' >&2
    exit 1
fi
cat "$case_pre/err"
[ "$(schema "$case_pre/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_pre/service.state")" = active ]
[ "$(cat "$case_pre/running.image")" = "$rollback_id" ]
grep -qx "Image=$rollback_id" "$case_pre/config/containers/systemd/gaudere-agent.container"
grep -q '^pre_swap_recovery=PASS$' "$case_pre/err"
grep -q '^service_restored=active$' "$case_pre/err"

# Failure after the state swap: automatically put exact v3 + old profile back,
# retain failed v4, and intentionally leave the service stopped for human review.
case_post="$workspace/fail-post"
make_fixture "$case_post"
if run_case "$case_post" success fail > "$case_post/out" 2> "$case_post/err"; then
    printf 'post-swap failure unexpectedly succeeded\n' >&2
    exit 1
fi
cat "$case_post/err"
[ "$(schema "$case_post/data/gaudere/state/state.db")" = 3 ]
[ "$(cat "$case_post/service.state")" = inactive ]
grep -qx "Image=$rollback_id" "$case_post/config/containers/systemd/gaudere-agent.container"
grep -q '^transaction_rollback=PASS$' "$case_post/err"
grep -q '^service_left=inactive$' "$case_post/err"
test -d "$case_post/data/gaudere/state.failed-v4-test"
[ "$(schema "$case_post/data/gaudere/state.failed-v4-test/state.db")" = 4 ]

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
grep -q '^transaction_rollback=PASS_ALREADY_RESTORED_BY_STAGE$' "$case_inner/err"

printf 'gaudere production schema v4 transaction test: PASS\n'
