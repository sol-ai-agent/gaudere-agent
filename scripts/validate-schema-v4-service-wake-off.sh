#!/bin/sh
set -eu

# PREP ONLY / NOT AUTHORIZED FOR PRODUCTION.
#
# This is the post-schema-v4 service-profile gate. It deliberately proves that a
# schema-v4 production state can run the normal OpenAI service profile while the
# explicit WakeIntent capability remains disabled. It submits no Task, provider
# work, or WakeIntent. A real run requires a separate explicit authorization flag.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
installer_default="$script_directory/install-openai-user-service.sh"
control_default="$script_directory/control-service.sh"
installer=${GAUDERE_OPENAI_INSTALLER:-$installer_default}
control_script=${GAUDERE_CONTROL_SCRIPT:-$control_default}
systemctl_command=${SYSTEMCTL:-systemctl}
journalctl_command=${JOURNALCTL:-journalctl}
podman_command=${PODMAN:-podman}
service_name=${GAUDERE_SERVICE_NAME:-}
state_directory=${GAUDERE_STATE_DIR:-}
runtime_image=${GAUDERE_RUNTIME_IMAGE:-localhost/gaudere-agent:dev}
expected_image_id=${GAUDERE_EXPECTED_RUNTIME_IMAGE_ID:-}
test_mode=${GAUDERE_TEST_MODE:-0}
authorization=${GAUDERE_SCHEMA_V4_WAKE_OFF_AUTHORIZATION:-}
representative_task=${1:-}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
source_profile=deploy/quadlet/gaudere-agent-openai.container.in
target_profile="$quadlet_directory/gaudere-agent.container"
phase=preflight
workspace=
profile_backup=
profile_replaced=0
service_started=0
success=0

fail()
{
    printf 'gaudere schema v4 wake-off service gate: phase=%s: %s\n' \
        "$phase" "$*" >&2
    exit 1
}

[ "$#" -eq 1 ] || fail "usage: $0 REPRESENTATIVE_PROVIDER_TASK_ID"
[ -n "$representative_task" ] || fail "representative provider task ID must not be empty"
[ -n "$service_name" ] || fail "GAUDERE_SERVICE_NAME must be set explicitly"
[ -n "$state_directory" ] || fail "GAUDERE_STATE_DIR must be set explicitly"
case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac
if [ "$test_mode" = "0" ]; then
    [ "$authorization" = "AUTHORIZED_SCHEMA_V4_WAKE_OFF_GATE" ] \
        || fail "real execution requires GAUDERE_SCHEMA_V4_WAKE_OFF_AUTHORIZATION=AUTHORIZED_SCHEMA_V4_WAKE_OFF_GATE"
    [ "$installer" = "$installer_default" ] \
        || fail "installer override is restricted to synthetic test mode"
    [ "$control_script" = "$control_default" ] \
        || fail "control-script override is restricted to synthetic test mode"
    [ -n "$expected_image_id" ] \
        || fail "GAUDERE_EXPECTED_RUNTIME_IMAGE_ID must be set for a real gate"
fi

for command in cmp cp date flock grep install mktemp python3 rm sha256sum; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
command -v "$systemctl_command" >/dev/null 2>&1 \
    || fail "systemctl command not found: $systemctl_command"
command -v "$journalctl_command" >/dev/null 2>&1 \
    || fail "journalctl command not found: $journalctl_command"
command -v "$podman_command" >/dev/null 2>&1 \
    || fail "podman command not found: $podman_command"
[ -f "$installer" ] || fail "installer not found: $installer"
[ -f "$control_script" ] || fail "control helper not found: $control_script"
[ -f "$source_profile" ] || fail "OpenAI Quadlet template not found: $source_profile"
[ -d "$state_directory" ] || fail "state directory not found: $state_directory"
[ -f "$state_directory/state.db" ] || fail "state database not found"
[ -f "$target_profile" ] || fail "installed Quadlet not found: $target_profile"

state_database="$state_directory/state.db"
workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-v4-wake-off.XXXXXX")
profile_backup="$workspace/profile.before"
before_snapshot="$workspace/state.before.json"
after_snapshot="$workspace/state.after.json"
install -m 0600 "$target_profile" "$profile_backup"

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

restore_on_failure()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$success" = "1" ]; then
        rm -rf -- "$workspace"
        exit "$status"
    fi

    printf 'gaudere schema v4 wake-off service gate: recovery phase=%s\n' "$phase" >&2
    current=$(service_state)
    case "$current" in
        active|activating|reloading|deactivating)
            "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
            ;;
    esac
    current=$(service_state)
    if [ "$profile_replaced" = "1" ] && [ "$current" = "inactive" ]; then
        install -m 0600 "$profile_backup" "$target_profile"
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
        printf 'gaudere schema v4 wake-off service gate: recovery_profile_restored=true\n' >&2
    elif [ "$profile_replaced" = "1" ]; then
        printf 'gaudere schema v4 wake-off service gate: recovery_profile_restored=false service_state=%s\n' \
            "$current" >&2
    fi
    printf 'gaudere schema v4 wake-off service gate: service_left=%s\n' "$(service_state)" >&2
    rm -rf -- "$workspace"
    exit "$status"
}
trap restore_on_failure EXIT HUP INT TERM

hold_lock_and_snapshot()
{
    output=$1
    exec 9>>"$state_database.lock"
    chmod 600 "$state_database.lock" 2>/dev/null || true
    flock -n 9 || fail "state database is currently owned"

    python3 - "$state_database" "$representative_task" > "$output" <<'PY'
import base64
import json
import sqlite3
import sys
import urllib.parse

path, representative = sys.argv[1], sys.argv[2]
uri = "file:" + urllib.parse.quote(path) + "?mode=ro"

def encode(value):
    if isinstance(value, bytes):
        return {"bytes_base64": base64.b64encode(value).decode("ascii")}
    return value

def quote_identifier(value):
    return '"' + value.replace('"', '""') + '"'

with sqlite3.connect(uri, uri=True) as db:
    version = db.execute("PRAGMA user_version").fetchone()[0]
    if version != 4:
        raise SystemExit(f"expected schema 4, found {version}")
    if [row[0] for row in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("SQLite integrity_check failed")

    objects = set(db.execute(
        "SELECT type,name FROM sqlite_master WHERE tbl_name='wake_intents' "
        "AND name NOT LIKE 'sqlite_autoindex_%'"
    ).fetchall())
    required = {
        ("table", "wake_intents"),
        ("index", "idx_wake_intents_scope_status_due"),
        ("trigger", "wake_intents_require_scheduled_insert"),
        ("trigger", "wake_intents_single_transition"),
        ("trigger", "wake_intents_prevent_delete"),
    }
    if objects != required:
        raise SystemExit("wake_intents objects do not match schema v4")
    wake_rows = db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0]
    if wake_rows != 0:
        raise SystemExit(f"wake_intents is not empty: {wake_rows}")

    task_blocker = db.execute(
        "SELECT id,status FROM tasks WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1"
    ).fetchone()
    if task_blocker:
        raise SystemExit(f"nonterminal Task blocks wake-off gate: {task_blocker[0]}:{task_blocker[1]}")
    action_blocker = db.execute(
        "SELECT id,status FROM actions WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1"
    ).fetchone()
    if action_blocker:
        raise SystemExit(f"nonterminal Action blocks wake-off gate: {action_blocker[0]}:{action_blocker[1]}")

    task = db.execute(
        "SELECT id,kind,status,attempts_started,result_content_type,result_output,"
        "result_failure_code,result_failure_message,result_metadata_content_type,result_metadata "
        "FROM tasks WHERE id=?", (representative,)
    ).fetchone()
    if task is None:
        raise SystemExit("representative provider Task is missing")
    if task[2] != 3:
        raise SystemExit("representative provider Task is not succeeded")
    if task[8] != "application/vnd.gaudere.provider-usage+json" or not task[9]:
        raise SystemExit("representative provider Task lacks durable usage metadata")

    provider_rows = db.execute(
        "SELECT COUNT(*) FROM budget_consumptions WHERE scope=?",
        ("provider.call:openai.responses",),
    ).fetchone()[0]

    schema_objects = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' AND tbl_name!='wake_intents' "
        "ORDER BY type,name"
    ).fetchall()
    tables = [row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' AND name!='wake_intents' ORDER BY name"
    )]
    contents = {}
    for table in tables:
        quoted = quote_identifier(table)
        columns = db.execute(f"PRAGMA table_xinfo({quoted})").fetchall()
        rows = [[encode(value) for value in row]
                for row in db.execute(f"SELECT * FROM {quoted}").fetchall()]
        rows.sort(key=lambda row: json.dumps(
            row, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
        contents[table] = {"columns": columns, "rows": rows}

    result = {
        "schema": version,
        "wake_rows": wake_rows,
        "provider_rows": provider_rows,
        "representative_task": task,
        "objects": schema_objects,
        "tables": contents,
    }
print(json.dumps(result, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
PY

    flock -u 9
    exec 9>&-
}

snapshot_value()
{
    key=$1
    file=$2
    python3 - "$key" "$file" <<'PY'
import json, sys
with open(sys.argv[2], encoding="utf-8") as source:
    print(json.load(source)[sys.argv[1]])
PY
}

observe_live()
{
    label=$1
    [ "$(service_state)" = "active" ] || fail "$service_name is not active during $label"

    budget=$(sh "$control_script" budget)
    printf '%s\n' "$budget"
    printf '%s\n' "$budget" | grep -qx 'provider_enabled=true' \
        || fail "provider capability is not enabled during $label"
    expected_rows=$(snapshot_value provider_rows "$before_snapshot")
    printf '%s\n' "$budget" | grep -qx "total_used=$expected_rows" \
        || fail "provider durable budget changed during $label"

    task=$(sh "$control_script" task "$representative_task")
    printf '%s\n' "$task"
    printf '%s\n' "$task" | grep -qx 'status=succeeded' \
        || fail "representative Task is not succeeded during $label"
    printf '%s\n' "$task" | grep -qx \
        'result_metadata_content_type="application/vnd.gaudere.provider-usage+json"' \
        || fail "representative Task usage metadata is not observable during $label"

    wake_probe="__gaudere_schema_v4_wake_off_probe__"
    if wake_output=$(sh "$control_script" wake "$wake_probe" 2>&1); then
        printf '%s\n' "$wake_output" >&2
        fail "observational wake lookup unexpectedly succeeded during $label"
    fi
    printf '%s\n' "$wake_output"
    printf '%s\n' "$wake_output" | grep -q \
        'explicit wake capability is not enabled in this service' \
        || fail "wake lookup did not prove disabled capability during $label"
}

printf 'status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION\n'
phase=offline-preflight
[ "$(service_state)" = "inactive" ] \
    || fail "$service_name must report exactly inactive before the gate"
hold_lock_and_snapshot "$before_snapshot"
printf 'schema_before=4\n'
printf 'wake_rows_before=%s\n' "$(snapshot_value wake_rows "$before_snapshot")"
printf 'provider_budget_rows_before=%s\n' "$(snapshot_value provider_rows "$before_snapshot")"

phase=image-fence
observed_image_id=$(
    "$podman_command" image inspect --format '{{.Id}}' "$runtime_image" 2>/dev/null \
        || true
)
[ -n "$observed_image_id" ] || fail "runtime image does not resolve: $runtime_image"
if [ -n "$expected_image_id" ]; then
    [ "$observed_image_id" = "$expected_image_id" ] \
        || fail "runtime image ID drift: expected $expected_image_id found $observed_image_id"
fi
printf 'runtime_image_id=%s\n' "$observed_image_id"

phase=profile-install
sh "$installer"
profile_replaced=1
cmp "$source_profile" "$target_profile" \
    || fail "installed profile differs from reviewed OpenAI template"
if grep -q -- '--wake-intents' "$target_profile"; then
    fail "installed profile contains --wake-intents"
fi
printf 'profile_wake_flag=absent\n'

phase=first-live-probe
started_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
"$systemctl_command" --user start "$service_name"
service_started=1
observe_live first-live-probe
first_journal=$(
    "$journalctl_command" --user -u "$service_name" --since "$started_at" --no-pager 2>&1 \
        || true
)
printf '%s\n' "$first_journal"
if printf '%s\n' "$first_journal" | grep -q 'explicit wake enabled'; then
    fail "service log reports explicit wake enabled"
fi

phase=safe-stop-audit
"$systemctl_command" --user stop "$service_name"
service_started=0
[ "$(service_state)" = "inactive" ] || fail "service did not become inactive for offline audit"
stop_journal=$(
    "$journalctl_command" --user -u "$service_name" --since "$started_at" --no-pager 2>&1 \
        || true
)
printf '%s\n' "$stop_journal"
printf '%s\n' "$stop_journal" | grep -q 'gaudere-agent: safe' \
    || fail "normal probe stop lacks safe shutdown evidence"
if printf '%s\n' "$stop_journal" | grep -q 'explicit wake enabled'; then
    fail "service log reports explicit wake enabled"
fi
hold_lock_and_snapshot "$after_snapshot"
cmp "$before_snapshot" "$after_snapshot" \
    || fail "durable non-wake state changed during wake-off live probe"
printf 'schema_after_probe=4\n'
printf 'wake_rows_after_probe=0\n'
printf 'durable_state_identity=PASS\n'

phase=final-restart
final_started_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
"$systemctl_command" --user start "$service_name"
service_started=1
observe_live final-restart
final_journal=$(
    "$journalctl_command" --user -u "$service_name" --since "$final_started_at" --no-pager 2>&1 \
        || true
)
printf '%s\n' "$final_journal"
if printf '%s\n' "$final_journal" | grep -q 'explicit wake enabled'; then
    fail "final service log reports explicit wake enabled"
fi
[ "$(service_state)" = "active" ] || fail "service is not active after final restart"
cmp "$source_profile" "$target_profile" || fail "installed profile drifted after restart"
final_image_id=$(
    "$podman_command" image inspect --format '{{.Id}}' "$runtime_image" 2>/dev/null \
        || true
)
[ "$final_image_id" = "$observed_image_id" ] || fail "runtime image ID drifted during gate"

success=1
trap - EXIT HUP INT TERM
rm -rf -- "$workspace"
printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'service_final=active\n'
printf 'gaudere schema v4 wake-off service gate: PASS\n'
