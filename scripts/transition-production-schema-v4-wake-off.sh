#!/bin/sh
set -eu

# PREP ONLY / NOT AUTHORIZED FOR PRODUCTION.
#
# One production transaction joins the already reviewed stopped-state v3->v4
# staging engine to the schema-v4 wake-off service gate. There is deliberately no
# externally visible half-way success: either the candidate service reaches the
# final wake-off proof, or recovery restores the original schema-v3 state/profile.
#
# This script never enables --wake-intents, submits provider work, consumes a
# provider permit, or publishes a port. A real run requires a one-shot explicit
# authorization token supplied by the operator after a separate GO decision.

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
default_deploy="$script_directory/deploy-schema-v4.sh"
default_wake_gate="$script_directory/validate-schema-v4-service-wake-off.sh"
default_control="$script_directory/control-service.sh"
deploy_script=${GAUDERE_SCHEMA_V4_DEPLOY_SCRIPT:-$default_deploy}
wake_gate=${GAUDERE_SCHEMA_V4_WAKE_OFF_GATE:-$default_wake_gate}
control_script=${GAUDERE_CONTROL_SCRIPT:-$default_control}
systemctl_command=${SYSTEMCTL:-systemctl}
podman_command=${PODMAN:-podman}
test_mode=${GAUDERE_TEST_MODE:-0}
authorization=${GAUDERE_SCHEMA_V4_PRODUCTION_AUTHORIZATION:-}

state_directory=${GAUDERE_STATE_DIR:-}
backup_directory=${GAUDERE_BACKUP_DIR:-}
service_name=${GAUDERE_SERVICE_NAME:-}
container_name=${GAUDERE_CONTAINER:-gaudere-agent}
candidate_image=${GAUDERE_CANDIDATE_IMAGE:-}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-}
expected_candidate_id=${GAUDERE_EXPECTED_CANDIDATE_ID:-}
rollback_image=${GAUDERE_ROLLBACK_IMAGE:-}
expected_rollback_id=${GAUDERE_EXPECTED_ROLLBACK_ID:-}
representative_task=${1:-}

phase=preflight
workspace=
phase_file=
profile_backup=
profile_target=
v3_snapshot=
snapshot_ready=0
recovery_armed=0
service_stop_attempted=0
crossed_swap=0
committed=0
rollback_directory=
failed_state_directory=
deployment_backup=

fail()
{
    printf 'gaudere production schema v4 transaction: phase=%s: %s\n' \
        "$phase" "$*" >&2
    exit 1
}

normalize_image_id()
{
    value=$1
    case "$value" in
        sha256:*) digest=${value#sha256:} ;;
        *) digest=$value ;;
    esac
    case "$digest" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#digest}" -eq 64 ] || return 1
    printf 'sha256:%s\n' "$digest"
}

valid_commit()
{
    value=$1
    case "$value" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#value}" -eq 40 ]
}

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

resolve_image_id()
{
    reference=$1
    "$podman_command" image exists "$reference" >/dev/null 2>&1 \
        || return 1
    raw=$("$podman_command" image inspect --format '{{.Id}}' "$reference" 2>/dev/null) \
        || return 1
    normalize_image_id "$raw"
}

running_image_id()
{
    raw=$(
        "$podman_command" container inspect --format '{{.Image}}' "$container_name" \
            2>/dev/null || true
    )
    [ -n "$raw" ] || return 1
    normalize_image_id "$raw"
}

budget_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
}

profile_image_ref()
{
    file=$1
    sed -n 's/^Image=//p' "$file"
}

set_phase()
{
    phase=$1
    if [ -n "$phase_file" ]; then
        printf '%s\n' "$phase" > "$phase_file.tmp"
        mv "$phase_file.tmp" "$phase_file"
    fi
    printf 'transaction_phase=%s\n' "$phase"
}

schema_version()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse
uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    print(db.execute("PRAGMA user_version").fetchone()[0])
PY
}

snapshot_v3()
{
    database=$1
    output=$2
    python3 - "$database" > "$output" <<'PY'
import base64
import json
import sqlite3
import sys
import urllib.parse

path = sys.argv[1]
uri = "file:" + urllib.parse.quote(path) + "?mode=ro"

def enc(value):
    if isinstance(value, bytes):
        return {"bytes_base64": base64.b64encode(value).decode("ascii")}
    return value

def qi(value):
    return '"' + value.replace('"', '""') + '"'

with sqlite3.connect(uri, uri=True) as db:
    if db.execute("PRAGMA user_version").fetchone()[0] != 3:
        raise SystemExit("expected schema 3")
    if [row[0] for row in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("SQLite integrity_check failed")
    wake = db.execute(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE name='wake_intents' OR tbl_name='wake_intents'"
    ).fetchone()[0]
    if wake != 0:
        raise SystemExit("schema 3 unexpectedly contains wake-intent objects")
    blocker = db.execute(
        "SELECT id,status FROM tasks WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1"
    ).fetchone()
    if blocker:
        raise SystemExit(f"nonterminal Task blocks transition: {blocker[0]}:{blocker[1]}")
    blocker = db.execute(
        "SELECT id,status FROM actions WHERE status IN (0,1,2) ORDER BY rowid LIMIT 1"
    ).fetchone()
    if blocker:
        raise SystemExit(f"nonterminal Action blocks transition: {blocker[0]}:{blocker[1]}")

    objects = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"
    ).fetchall()
    tables = [row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name"
    )]
    contents = {}
    for table in tables:
        quoted = qi(table)
        columns = db.execute(f"PRAGMA table_xinfo({quoted})").fetchall()
        rows = [[enc(value) for value in row]
                for row in db.execute(f"SELECT * FROM {quoted}").fetchall()]
        rows.sort(key=lambda row: json.dumps(
            row, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
        contents[table] = {"columns": columns, "rows": rows}

    provider_rows = db.execute(
        "SELECT COUNT(*) FROM budget_consumptions WHERE scope=?",
        ("provider.call:openai.responses",),
    ).fetchone()[0]
    if provider_rows != 3:
        raise SystemExit(f"expected exactly 3 provider budget rows, found {provider_rows}")

    print(json.dumps({
        "objects": objects,
        "tables": contents,
        "provider_rows": provider_rows,
    }, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
PY
}

verify_v3_snapshot()
{
    [ "$snapshot_ready" = "1" ] || return 1
    current="$workspace/v3.current.json"
    snapshot_v3 "$state_directory/state.db" "$current" || return 1
    cmp -s "$v3_snapshot" "$current"
}

verify_basic_v3()
{
    database=$1
    python3 - "$database" <<'PY'
import sqlite3
import sys
import urllib.parse
uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    if db.execute("PRAGMA user_version").fetchone()[0] != 3:
        raise SystemExit(1)
    if [row[0] for row in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit(1)
    if db.execute(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE name='wake_intents' OR tbl_name='wake_intents'"
    ).fetchone()[0] != 0:
        raise SystemExit(1)
PY
}

restore_profile()
{
    [ -f "$profile_backup" ] || return 1
    install -m 0600 "$profile_backup" "$profile_target" || return 1
    "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || return 1
    cmp -s "$profile_backup" "$profile_target"
}

old_profile_resolves_rollback()
{
    reference=$(profile_image_ref "$profile_backup")
    [ -n "$reference" ] || return 1
    [ "$(resolve_image_id "$reference" || true)" = "$expected_rollback_id" ]
}

stop_for_recovery()
{
    current=$(service_state)
    case "$current" in
        inactive) return 0 ;;
        active|activating|reloading|deactivating)
            "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
            ;;
        *) return 1 ;;
    esac
    [ "$(service_state)" = "inactive" ]
}

post_swap_rollback()
{
    printf 'transaction_rollback=STARTED\n' >&2
    stop_for_recovery || {
        printf 'transaction_rollback=MANUAL_REVIEW service_not_inactive\n' >&2
        return 1
    }

    [ -n "$rollback_directory" ] && [ -d "$rollback_directory" ] \
        || {
            printf 'transaction_rollback=MANUAL_REVIEW rollback_directory_missing\n' >&2
            return 1
        }
    [ -n "$failed_state_directory" ] \
        || {
            printf 'transaction_rollback=MANUAL_REVIEW failed_state_path_missing\n' >&2
            return 1
        }
    [ ! -e "$failed_state_directory" ] \
        || {
            printf 'transaction_rollback=MANUAL_REVIEW failed_state_path_occupied\n' >&2
            return 1
        }

    exec 8>>"$state_directory/state.db.lock"
    chmod 600 "$state_directory/state.db.lock" 2>/dev/null || true
    flock -n 8 || {
        exec 8>&-
        printf 'transaction_rollback=MANUAL_REVIEW installed_state_owned\n' >&2
        return 1
    }

    installed_schema=$(schema_version "$state_directory/state.db" 2>/dev/null || true)
    rollback_schema=$(schema_version "$rollback_directory/state.db" 2>/dev/null || true)
    if [ "$installed_schema" != "4" ] || [ "$rollback_schema" != "3" ]; then
        flock -u 8 || true
        exec 8>&-
        printf 'transaction_rollback=MANUAL_REVIEW unexpected_schema_layout installed=%s rollback=%s\n' \
            "$installed_schema" "$rollback_schema" >&2
        return 1
    fi

    if ! mv "$state_directory" "$failed_state_directory"; then
        flock -u 8 || true
        exec 8>&-
        printf 'transaction_rollback=MANUAL_REVIEW cannot_retain_failed_v4\n' >&2
        return 1
    fi
    flock -u 8 || true
    exec 8>&-

    if ! mv "$rollback_directory" "$state_directory"; then
        printf 'transaction_rollback=MANUAL_REVIEW cannot_restore_v3\n' >&2
        return 1
    fi

    verify_v3_snapshot || {
        printf 'transaction_rollback=MANUAL_REVIEW restored_v3_mismatch\n' >&2
        return 1
    }
    restore_profile || {
        printf 'transaction_rollback=MANUAL_REVIEW profile_restore_failed\n' >&2
        return 1
    }
    old_profile_resolves_rollback || {
        printf 'transaction_rollback=MANUAL_REVIEW rollback_image_identity_failed\n' >&2
        return 1
    }

    printf 'transaction_rollback=PASS\n' >&2
    printf 'restored_schema=3\n' >&2
    printf 'retained_failed_v4=%s\n' "$failed_state_directory" >&2
    printf 'service_left=inactive\n' >&2
    return 0
}

pre_swap_recovery()
{
    current=$(service_state)
    if [ "$current" != "inactive" ]; then
        stop_for_recovery || {
            printf 'pre_swap_recovery=MANUAL_REVIEW service_state=%s\n' "$current" >&2
            return 1
        }
    fi

    if [ "$snapshot_ready" = "1" ]; then
        verify_v3_snapshot || {
            printf 'pre_swap_recovery=MANUAL_REVIEW v3_snapshot_mismatch\n' >&2
            return 1
        }
    else
        verify_basic_v3 "$state_directory/state.db" || {
            printf 'pre_swap_recovery=MANUAL_REVIEW v3_validation_failed\n' >&2
            return 1
        }
    fi

    restore_profile || {
        printf 'pre_swap_recovery=MANUAL_REVIEW profile_restore_failed\n' >&2
        return 1
    }
    old_profile_resolves_rollback || {
        printf 'pre_swap_recovery=MANUAL_REVIEW rollback_image_identity_failed\n' >&2
        return 1
    }

    if ! "$systemctl_command" --user start "$service_name" >/dev/null 2>&1; then
        printf 'pre_swap_recovery=MANUAL_REVIEW service_restart_failed\n' >&2
        return 1
    fi
    [ "$(service_state)" = "active" ] || {
        printf 'pre_swap_recovery=MANUAL_REVIEW service_not_active_after_restart\n' >&2
        return 1
    }
    actual=$(running_image_id || true)
    [ "$actual" = "$expected_rollback_id" ] || {
        "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
        printf 'pre_swap_recovery=MANUAL_REVIEW running_image_mismatch=%s\n' "$actual" >&2
        return 1
    }
    printf 'pre_swap_recovery=PASS\n' >&2
    printf 'service_restored=active\n' >&2
    return 0
}

recover()
{
    status=$?
    trap - EXIT HUP INT TERM

    if [ "$committed" = "1" ] || [ "$recovery_armed" != "1" ]; then
        exit "$status"
    fi

    printf 'gaudere production schema v4 transaction: recovery phase=%s\n' "$phase" >&2
    if [ "$crossed_swap" = "1" ]; then
        post_swap_rollback || true
    elif [ "$service_stop_attempted" = "1" ]; then
        pre_swap_recovery || true
    fi

    [ -z "$workspace" ] || printf 'transition_workspace=%s\n' "$workspace" >&2
    exit "$status"
}

[ "$#" -eq 1 ] || fail "usage: $0 REPRESENTATIVE_PROVIDER_TASK_ID"
[ -n "$representative_task" ] || fail "representative provider Task ID must not be empty"
case "$test_mode" in
    0|1) ;;
    *) fail "GAUDERE_TEST_MODE must be 0 or 1" ;;
esac

for required in "$state_directory" "$backup_directory" "$service_name" \
        "$candidate_image" "$expected_agent_ref" "$expected_core_ref" \
        "$expected_candidate_id" "$rollback_image" "$expected_rollback_id"; do
    [ -n "$required" ] || fail "all production transition environment values must be explicit"
done
valid_commit "$expected_agent_ref" || fail "invalid expected Agent revision"
valid_commit "$expected_core_ref" || fail "invalid expected Core revision"
[ "$(normalize_image_id "$expected_candidate_id" || true)" = "$expected_candidate_id" ] \
    || fail "expected candidate ID must use full sha256:<64-hex> form"
[ "$(normalize_image_id "$expected_rollback_id" || true)" = "$expected_rollback_id" ] \
    || fail "expected rollback ID must use full sha256:<64-hex> form"
[ "$expected_candidate_id" != "$expected_rollback_id" ] \
    || fail "candidate and rollback image IDs must differ"

if [ "$test_mode" = "0" ]; then
    [ "$authorization" = "AUTHORIZED_PRODUCTION_SCHEMA_V4_WAKE_OFF" ] \
        || fail "real execution requires GAUDERE_SCHEMA_V4_PRODUCTION_AUTHORIZATION=AUTHORIZED_PRODUCTION_SCHEMA_V4_WAKE_OFF"
    [ "$deploy_script" = "$default_deploy" ] \
        || fail "deploy-script override is restricted to synthetic test mode"
    [ "$wake_gate" = "$default_wake_gate" ] \
        || fail "wake-gate override is restricted to synthetic test mode"
    [ "$control_script" = "$default_control" ] \
        || fail "control override is restricted to synthetic test mode"
fi

for command in cmp flock grep install mkdir mktemp mv python3 realpath rm sed sha256sum tail; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
[ -f "$deploy_script" ] || fail "deploy script not found: $deploy_script"
[ -f "$wake_gate" ] || fail "wake-off gate not found: $wake_gate"
[ -f "$control_script" ] || fail "control script not found: $control_script"
[ -d "$state_directory" ] && [ -f "$state_directory/state.db" ] \
    || fail "production state is missing"

state_directory=$(realpath "$state_directory")
backup_directory=$(realpath -m "$backup_directory")
state_parent=$(dirname "$state_directory")
transition_root="$state_parent/.schema-v4-transitions"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
phase_file="$workspace/phase"
profile_target="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd/gaudere-agent.container"
profile_backup="$workspace/profile.before"
v3_snapshot="$workspace/v3.before.json"
[ -f "$profile_target" ] || fail "installed service profile not found: $profile_target"
install -m 0600 "$profile_target" "$profile_backup"
sha256sum "$profile_backup" > "$workspace/profile.before.sha256"

set_phase live-preflight
[ "$(service_state)" = "active" ] || fail "$service_name must be active before transition"
[ "$(resolve_image_id "$candidate_image" || true)" = "$expected_candidate_id" ] \
    || fail "candidate image tag does not resolve to the approved image ID"
[ "$(resolve_image_id "$rollback_image" || true)" = "$expected_rollback_id" ] \
    || fail "rollback image tag does not resolve to the approved image ID"
[ "$(running_image_id || true)" = "$expected_rollback_id" ] \
    || fail "running production container is not the approved pre-v4 rollback image"
old_profile_resolves_rollback \
    || fail "installed pre-v4 profile does not resolve to the approved rollback image"

before_budget=$(sh "$control_script" budget)
printf '%s\n' "$before_budget"
[ "$(budget_value provider_enabled "$before_budget")" = "true" ] \
    || fail "provider capability is not enabled before transition"
[ "$(budget_value total_used "$before_budget")" = "3" ] \
    || fail "transition requires exactly three durable provider consumptions"

before_task=$(sh "$control_script" task "$representative_task")
printf '%s\n' "$before_task"
printf '%s\n' "$before_task" | grep -qx 'status=succeeded' \
    || fail "representative provider Task is not succeeded"
printf '%s\n' "$before_task" | grep -qx \
    'result_metadata_content_type="application/vnd.gaudere.provider-usage+json"' \
    || fail "representative provider Task lacks durable usage metadata"

recovery_armed=1
trap recover EXIT HUP INT TERM
set_phase stopping
service_stop_attempted=1
"$systemctl_command" --user stop "$service_name"
[ "$(service_state)" = "inactive" ] || fail "$service_name did not become inactive"

set_phase stopped-v3-snapshot
exec 9>>"$state_directory/state.db.lock"
chmod 600 "$state_directory/state.db.lock" 2>/dev/null || true
flock -n 9 || fail "stopped production state is still owned"
snapshot_v3 "$state_directory/state.db" "$v3_snapshot" \
    || fail "stopped schema-v3 source is not quiescent and exact"
snapshot_ready=1
flock -u 9
exec 9>&-
printf 'v3_snapshot=PASS\n'

set_phase deploying-v4
if ! deployment_output=$(\
    GAUDERE_STATE_DIR="$state_directory" \
    GAUDERE_BACKUP_DIR="$backup_directory" \
    GAUDERE_SERVICE_NAME="$service_name" \
    GAUDERE_CANDIDATE_IMAGE="$candidate_image" \
    GAUDERE_EXPECTED_AGENT_REF="$expected_agent_ref" \
    GAUDERE_EXPECTED_CORE_REF="$expected_core_ref" \
    GAUDERE_EXPECTED_CANDIDATE_ID="$expected_candidate_id" \
    GAUDERE_ROLLBACK_IMAGE="$rollback_image" \
    GAUDERE_EXPECTED_ROLLBACK_ID="$expected_rollback_id" \
    GAUDERE_TEST_MODE="$test_mode" \
    SYSTEMCTL="$systemctl_command" PODMAN="$podman_command" \
        sh "$deploy_script" 2>&1); then
    printf '%s\n' "$deployment_output" | tee "$workspace/deploy.out" >&2
    if printf '%s\n' "$deployment_output" | grep -q '^automatic_rollback=STARTED$'; then
        crossed_swap=1
    fi
    fail "schema-v4 state deployment did not complete"
fi
printf '%s\n' "$deployment_output" | tee "$workspace/deploy.out"
printf '%s\n' "$deployment_output" | grep -q \
    '^gaudere staged schema v4 deployment: PREPARED$' \
    || fail "state deployment did not report PREPARED"
rollback_directory=$(printf '%s\n' "$deployment_output" | sed -n 's/^rollback_directory=//p' | tail -n 1)
failed_state_directory=$(printf '%s\n' "$deployment_output" | sed -n 's/^failed_state_directory_if_needed=//p' | tail -n 1)
deployment_backup=$(printf '%s\n' "$deployment_output" | sed -n 's/^backup=//p' | tail -n 1)
[ -n "$rollback_directory" ] && [ -d "$rollback_directory" ] \
    || fail "state deployment did not retain a v3 rollback directory"
[ -n "$failed_state_directory" ] \
    || fail "state deployment did not report a failed-v4 retention path"
[ -n "$deployment_backup" ] && [ -f "$deployment_backup" ] \
    || fail "state deployment did not retain its fresh backup"
crossed_swap=1
set_phase v4-prepared

set_phase wake-off-service-gate
if ! wake_output=$(\
    GAUDERE_STATE_DIR="$state_directory" \
    GAUDERE_SERVICE_NAME="$service_name" \
    GAUDERE_RUNTIME_IMAGE="$candidate_image" \
    GAUDERE_EXPECTED_RUNTIME_IMAGE_ID="$expected_candidate_id" \
    GAUDERE_SCHEMA_V4_WAKE_OFF_AUTHORIZATION=AUTHORIZED_SCHEMA_V4_WAKE_OFF_GATE \
    GAUDERE_TEST_MODE="$test_mode" \
    SYSTEMCTL="$systemctl_command" PODMAN="$podman_command" \
        sh "$wake_gate" "$representative_task" 2>&1); then
    printf '%s\n' "$wake_output" | tee "$workspace/wake-off.out" >&2
    fail "schema-v4 wake-off service gate failed"
fi
printf '%s\n' "$wake_output" | tee "$workspace/wake-off.out"
printf '%s\n' "$wake_output" | grep -q \
    '^gaudere schema v4 wake-off service gate: PASS$' \
    || fail "wake-off gate did not report PASS"
printf '%s\n' "$wake_output" | grep -q '^provider_effects=0$' \
    || fail "wake-off gate did not prove zero provider effects"
printf '%s\n' "$wake_output" | grep -q '^wake_effects=0$' \
    || fail "wake-off gate did not prove zero wake effects"
printf '%s\n' "$wake_output" | grep -q '^runtime_image_identity=PASS$' \
    || fail "wake-off gate did not prove running image identity"

set_phase final-live-audit
[ "$(service_state)" = "active" ] || fail "candidate service is not active after wake-off gate"
[ "$(running_image_id || true)" = "$expected_candidate_id" ] \
    || fail "final running service is not the approved candidate image"

after_budget=$(sh "$control_script" budget)
printf '%s\n' "$after_budget"
[ "$(budget_value provider_enabled "$after_budget")" = "true" ] \
    || fail "provider capability is not enabled after transition"
[ "$(budget_value total_used "$after_budget")" = "3" ] \
    || fail "provider durable budget changed during transition"

after_task=$(sh "$control_script" task "$representative_task")
printf '%s\n' "$after_task"
printf '%s\n' "$after_task" | grep -qx 'status=succeeded' \
    || fail "representative provider Task changed during transition"
printf '%s\n' "$after_task" | grep -qx \
    'result_metadata_content_type="application/vnd.gaudere.provider-usage+json"' \
    || fail "representative usage metadata is missing after transition"

wake_probe="__gaudere_production_v4_transaction_probe__"
if wake_observation=$(sh "$control_script" wake "$wake_probe" 2>&1); then
    printf '%s\n' "$wake_observation" >&2
    fail "final observational wake lookup unexpectedly succeeded"
fi
printf '%s\n' "$wake_observation"
printf '%s\n' "$wake_observation" | grep -q \
    'explicit wake capability is not enabled in this service' \
    || fail "final wake observation did not prove disabled capability"

set_phase committed
committed=1
recovery_armed=0
trap - EXIT HUP INT TERM
printf 'production_schema=4\n'
printf 'candidate_image_id=%s\n' "$expected_candidate_id"
printf 'rollback_image_id=%s\n' "$expected_rollback_id"
printf 'rollback_directory=%s\n' "$rollback_directory"
printf 'backup=%s\n' "$deployment_backup"
printf 'provider_effects=0\n'
printf 'wake_effects=0\n'
printf 'service_final=active\n'
printf 'wake_capability_active=false\n'
printf 'transition_workspace=%s\n' "$workspace"
printf 'gaudere production schema v4 wake-off transaction: PASS\n'
