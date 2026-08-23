#!/bin/sh
set -eu

# PREP ONLY until Sol + Bertrand separately authorize the isolated host-downtime proof.
# This phase clones production state consistently, resets only wake_intents in the
# isolated copy, starts a Network=none/no-secret staging Quadlet, and accepts the
# frozen canonical source exactly once plus one duplicate idempotency proof.

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
source_task=production-reflection-wake-source-first
expected_delay_seconds=3600
expected_delay_ms=3600000
expected_reason='Resume after a one-hour production observation window to verify that the active pre-wake runtime leaves durable, interpretable evidence, journal the result, and identify the single reliability condition that should gate any future WakeIntent enablement. This advances cooperation reliability without spending scarce provider budget on a more ambitious step.'
frozen_runtime_image=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
frozen_agent_ref=4e6cb09467456f38377bd8610e1ac534c7705380
frozen_core_ref=1316cf68db93e4c91a7bd79fbd289b8f382f8659

phase=preflight
wake_accepted=0
success=0

fail()
{
    printf 'gaudere wake host-downtime arm: phase=%s: %s\n' "$phase" "$*" >&2
    exit 1
}

report_value()
{
    key=$1
    body=$2
    printf '%s\n' "$body" | sed -n "s/^${key}=//p" | tail -n 1
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
            "SELECT type,name,tbl_name,sql FROM sqlite_master "
            "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"
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

[ "$#" -eq 1 ] || fail "usage: $0 --prepare-after-explicit-host-downtime-go"
[ "$1" = "--prepare-after-explicit-host-downtime-go" ] \
    || fail "explicit isolated host-downtime authorization argument is required"

for command in awk cmp grep install mkdir python3 sed sha256sum sync; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
command -v "$podman_command" >/dev/null 2>&1 || fail "podman command not found"
command -v "$systemctl_command" >/dev/null 2>&1 || fail "systemctl command not found"
[ -f "$control_script" ] || fail "control helper not found"
[ -r "$boot_id_file" ] || fail "boot-id source is not readable"
[ -f "$production_database" ] || fail "production state database not found"
[ "$(service_state "$production_service")" = "active" ] || fail "production service is not active"
[ ! -e "$proof_root" ] || fail "proof directory already exists: $proof_root"
[ ! -e "$staging_root" ] || fail "staging root already exists: $staging_root"
[ ! -e "$staging_profile" ] || fail "staging Quadlet already exists"
[ "$(service_state "$staging_service")" != "active" ] || fail "staging service is already active"

production_image=$(container_image "$production_container") || fail "cannot inspect production runtime image"
[ "$production_image" = "$frozen_runtime_image" ] || fail "production runtime image drift"
agent_ref=$("$podman_command" image inspect --format '{{index .Labels "io.gaudere.agent.revision"}}' "$production_image" 2>/dev/null) \
    || fail "cannot inspect production Agent provenance"
core_ref=$("$podman_command" image inspect --format '{{index .Labels "io.gaudere.core.revision"}}' "$production_image" 2>/dev/null) \
    || fail "cannot inspect production Core provenance"
[ "$agent_ref" = "$frozen_agent_ref" ] || fail "production Agent provenance drift"
[ "$core_ref" = "$frozen_core_ref" ] || fail "production Core provenance drift"

if production_wake=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake" >&2
    fail "production WakeIntent is unexpectedly enabled"
fi
printf '%s\n' "$production_wake" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "production wake-status did not prove disabled capability"

production_budget=$(production_control budget)
printf '%s\n' "$production_budget"
[ "$(report_value provider_enabled "$production_budget")" = "true" ] || fail "production provider is not enabled"
[ "$(report_value total_used "$production_budget")" = "4" ] || fail "production provider total must be exactly 4"

source_report=$(production_control task "$source_task") || fail "canonical source Task is not inspectable"
printf '%s\n' "$source_report"
SOURCE_REPORT="$source_report" python3 - "$source_task" "$expected_reason" "$expected_delay_seconds" <<'PY'
import json, os, sys
task_id, reason, delay = sys.argv[1], sys.argv[2], int(sys.argv[3])
fields = {}
for raw in os.environ["SOURCE_REPORT"].splitlines():
    if "=" in raw:
        fields[raw.split("=",1)[0]] = raw.split("=",1)[1]
if json.loads(fields.get("id",'""')) != task_id:
    raise SystemExit("source Task ID mismatch")
if json.loads(fields.get("kind",'""')) != "cognition.reflect.v1" or fields.get("status") != "succeeded":
    raise SystemExit("source Task is not succeeded cognition.reflect.v1")
if json.loads(fields.get("result_content_type",'""')) != "application/vnd.gaudere.cognition-decision+json":
    raise SystemExit("source result content type mismatch")
decision = json.loads(json.loads(fields["result_output"]))
expected = {
    "schema":"gaudere.cognition.decision.v1",
    "decision":"propose_wake",
    "reason":reason,
    "wake_after_seconds":delay,
}
if decision != expected:
    raise SystemExit("source decision differs from frozen canonical proposal")
PY

phase=isolation
production_resolved=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$production_database")
staging_resolved=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$staging_database")
[ "$production_resolved" != "$staging_resolved" ] || fail "staging database resolves to production database"

install -d -m 0700 "$(dirname "$proof_root")" "$quadlet_directory"
mkdir -m 0700 "$proof_root"
mkdir -m 0700 "$staging_root" "$staging_state_directory"
printf '%s\n' "$source_report" > "$proof_root/source-task.report"
printf '%s\n' "$production_budget" > "$proof_root/production-budget-before.report"

recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    [ "$success" = "1" ] && exit "$status"
    "$systemctl_command" --user stop "$staging_service" >/dev/null 2>&1 || true
    if [ "$wake_accepted" = "1" ]; then
        printf 'gaudere wake host-downtime arm: staged wake slot may be consumed; staging stopped and evidence retained for manual review\n' >&2
        printf 'gaudere wake host-downtime arm: retained proof=%s staging=%s\n' "$proof_root" "$staging_root" >&2
    else
        rm -f "$staging_profile" >/dev/null 2>&1 || true
        "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
        rm -rf "$staging_root" "$proof_root" >/dev/null 2>&1 || true
        printf 'gaudere wake host-downtime arm: pre-acceptance failure removed isolated staging artifacts\n' >&2
    fi
    exit "$status"
}
trap recover EXIT HUP INT TERM

snapshot_database "$production_database" "$proof_root/production-before.json" all
snapshot_database "$production_database" "$proof_root/production-nonwake-before.json" nonwake

phase=clone
python3 - "$production_database" "$staging_database" <<'PY'
import os, pathlib, sqlite3, sys
source, target = map(pathlib.Path, sys.argv[1:])
source = source.resolve()
target = target.resolve()
if source == target:
    raise SystemExit("source and staging database are identical")
uri = source.as_uri() + "?mode=ro"
with sqlite3.connect(uri, uri=True) as src, sqlite3.connect(target) as dst:
    src.execute("PRAGMA query_only=ON")
    src.backup(dst)
os.chmod(target, 0o600)
PY
snapshot_database "$staging_database" "$proof_root/staging-clone-before.json" all
cmp "$proof_root/production-before.json" "$proof_root/staging-clone-before.json" >/dev/null \
    || fail "consistent staging clone differs logically from production snapshot"

phase=reset-isolated-wake
python3 - "$staging_database" "$source_task" <<'PY'
import sqlite3, sys
path, source = sys.argv[1:]
def qi(v):
    return '"' + v.replace('"','""') + '"'
with sqlite3.connect(path) as db:
    db.execute("PRAGMA foreign_keys=ON")
    if db.execute("PRAGMA user_version").fetchone()[0] != 4:
        raise SystemExit("staging schema is not 4")
    row = db.execute(
        "SELECT id,source_id,status FROM wake_intents ORDER BY rowid"
    ).fetchall()
    if row != [(source, source, 1)]:
        raise SystemExit(f"expected exactly the completed production wake row, got {row!r}")
    schema_before = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"
    ).fetchall()
    triggers = db.execute(
        "SELECT name,sql FROM sqlite_master "
        "WHERE type='trigger' AND tbl_name='wake_intents' ORDER BY name"
    ).fetchall()
    db.execute("BEGIN IMMEDIATE")
    for name, _ in triggers:
        db.execute("DROP TRIGGER " + qi(name))
    db.execute("DELETE FROM wake_intents")
    for _, sql in triggers:
        if not sql:
            raise SystemExit("wake trigger lacks SQL")
        db.execute(sql)
    db.commit()
    schema_after = db.execute(
        "SELECT type,name,tbl_name,sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"
    ).fetchall()
    if schema_after != schema_before:
        raise SystemExit("staging schema changed while resetting isolated wake slot")
    if db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0] != 0:
        raise SystemExit("isolated wake reset did not produce an empty scope")
    if [r[0] for r in db.execute("PRAGMA integrity_check")] != ["ok"]:
        raise SystemExit("staging integrity_check failed after wake reset")
PY

snapshot_database "$staging_database" "$proof_root/staging-nonwake-baseline.json" nonwake
cmp "$proof_root/production-nonwake-before.json" "$proof_root/staging-nonwake-baseline.json" >/dev/null \
    || fail "isolated wake reset changed non-wake durable state"

phase=profile
cat > "$proof_root/staging-profile.expected" <<EOF
[Unit]
Description=Gaudere WakeIntent host-downtime staging
Documentation=https://github.com/sol-ai-agent/gaudere-agent/issues/81

[Container]
Image=$frozen_runtime_image
Pull=never
ContainerName=$staging_container
Exec=--state /var/lib/gaudere/state.db --control-socket $staging_socket --wake-intents
Network=none
UserNS=keep-id
Volume=$staging_state_directory:/var/lib/gaudere:Z

ReadOnly=true
ReadOnlyTmpfs=true
NoNewPrivileges=true
DropCapability=all
LogDriver=journald
PidsLimit=64
Memory=256m
StopSignal=SIGTERM
StopTimeout=30

[Service]
Restart=on-failure
TimeoutStopSec=45

[Install]
WantedBy=default.target
EOF
python3 - "$proof_root/staging-profile.expected" <<'PY'
import pathlib, sys
text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
for token in ("Network=none", "--wake-intents", "Pull=never", "WantedBy=default.target"):
    if token not in text:
        raise SystemExit("staging profile missing " + token)
for forbidden in ("--openai-", "Secret=", "PublishPort=", "Network=host"):
    if forbidden in text:
        raise SystemExit("staging profile contains forbidden capability: " + forbidden)
PY
install -m 0600 "$proof_root/staging-profile.expected" "$staging_profile"
"$systemctl_command" --user daemon-reload
enablement=$("$systemctl_command" --user is-enabled "$staging_service" 2>/dev/null || true)
case "$enablement" in
    enabled|enabled-runtime|generated|static|indirect) ;;
    *) fail "staging service is not wired for user-manager startup: ${enablement:-unknown}" ;;
esac
"$systemctl_command" --user start "$staging_service"
[ "$(service_state "$staging_service")" = "active" ] || fail "staging service did not start"
[ "$(container_image "$staging_container")" = "$frozen_runtime_image" ] || fail "staging runtime image drift"

phase=staging-empty
empty_status=$(staging_control wake-status) || fail "staging wake-status failed before acceptance"
printf '%s\n' "$empty_status"
[ "$(report_value record "$empty_status")" = "none" ] || fail "staging wake scope is not empty"
[ "$(report_value health "$empty_status")" = "empty" ] || fail "empty staging wake scope is not healthy"

staging_source=$(staging_control task "$source_task") || fail "canonical source is missing from staging clone"
[ "$staging_source" = "$source_report" ] || fail "staging source report differs from production source report"

phase=accept
acceptance=$(staging_control accept-wake "$source_task") || fail "staging WakeIntent acceptance failed"
wake_accepted=1
printf '%s\n' "$acceptance" | tee "$proof_root/acceptance.report"
[ "$(report_value acceptance "$acceptance")" = "accepted" ] || fail "first staging acceptance was not accepted"
[ "$(report_value status "$acceptance")" = "scheduled" ] || fail "staging wake is not scheduled"
accepted_at=$(report_value accepted_at_ms "$acceptance")
due_at=$(report_value due_at_ms "$acceptance")
case "$accepted_at" in ''|*[!0-9]*) fail "invalid accepted_at_ms" ;; esac
case "$due_at" in ''|*[!0-9]*) fail "invalid due_at_ms" ;; esac
[ $((due_at - accepted_at)) -eq "$expected_delay_ms" ] || fail "staging deadline delta is not 3600000 ms"

duplicate=$(staging_control accept-wake "$source_task") || fail "staging duplicate acceptance failed"
printf '%s\n' "$duplicate" | tee "$proof_root/duplicate.report"
[ "$(report_value acceptance "$duplicate")" = "duplicate" ] || fail "second staging acceptance was not duplicate"
[ "$(report_value accepted_at_ms "$duplicate")" = "$accepted_at" ] || fail "duplicate changed accepted_at"
[ "$(report_value due_at_ms "$duplicate")" = "$due_at" ] || fail "duplicate changed due_at"

scheduled=$(staging_control wake-status) || fail "staging wake-status failed after acceptance"
printf '%s\n' "$scheduled" | tee "$proof_root/scheduled.report"
[ "$(report_value status "$scheduled")" = "scheduled" ] || fail "staging wake is not scheduled"
[ "$(report_value source_task_id "$scheduled")" = "\"$source_task\"" ] || fail "scheduled source mismatch"
[ "$(report_value accepted_at_ms "$scheduled")" = "$accepted_at" ] || fail "scheduled accepted_at mismatch"
[ "$(report_value due_at_ms "$scheduled")" = "$due_at" ] || fail "scheduled due_at mismatch"
[ "$(report_value scheduler_coverage "$scheduled")" = "exact" ] || fail "staging scheduler coverage is not exact"

snapshot_database "$staging_database" "$proof_root/staging-nonwake-after-arm.json" nonwake
cmp "$proof_root/staging-nonwake-baseline.json" "$proof_root/staging-nonwake-after-arm.json" >/dev/null \
    || fail "staging acceptance changed non-wake durable state"

phase=production-invariant
snapshot_database "$production_database" "$proof_root/production-after-arm.json" all
cmp "$proof_root/production-before.json" "$proof_root/production-after-arm.json" >/dev/null \
    || fail "production durable state changed while preparing isolated staging"
[ "$(service_state "$production_service")" = "active" ] || fail "production service is not active after staging arm"
[ "$(container_image "$production_container")" = "$frozen_runtime_image" ] || fail "production runtime changed"
if production_wake_after=$(production_control wake-status 2>&1); then
    printf '%s\n' "$production_wake_after" >&2
    fail "production WakeIntent became enabled"
fi
printf '%s\n' "$production_wake_after" | grep -q 'explicit wake capability is not enabled in this service' \
    || fail "production wake capability invariant is ambiguous"
production_budget_after=$(production_control budget)
[ "$(report_value total_used "$production_budget_after")" = "4" ] || fail "production provider total changed"

boot_id=$(tr -d '\n' < "$boot_id_file")
case "$boot_id" in ''|*[!0-9a-fA-F-]*) fail "invalid boot id" ;; esac
profile_sha=$(sha256sum "$staging_profile" | awk '{print $1}')
cat > "$proof_root/phase-arm.meta" <<EOF
phase_arm=PASS
source_task=$source_task
runtime_image=$frozen_runtime_image
agent_ref=$frozen_agent_ref
core_ref=$frozen_core_ref
boot_id=$boot_id
accepted_at_ms=$accepted_at
due_at_ms=$due_at
expected_delay_ms=$expected_delay_ms
staging_profile_sha256=$profile_sha
staging_service=$staging_service
staging_container=$staging_container
staging_socket=$staging_socket
EOF
chmod 0600 "$proof_root/phase-arm.meta"
sync

success=1
trap - EXIT HUP INT TERM

printf 'source_task=%s\n' "$source_task"
printf 'accepted_at_ms=%s\n' "$accepted_at"
printf 'due_at_ms=%s\n' "$due_at"
printf 'deadline_delta_ms=%s\n' "$expected_delay_ms"
printf 'duplicate_deadline_identity=PASS\n'
printf 'staging_isolation=PASS\n'
printf 'staging_nonwake_state_unchanged=PASS\n'
printf 'staging_network=none\n'
printf 'staging_provider_capability=absent\n'
printf 'production_untouched=PASS\n'
printf 'production_provider_total=4\n'
printf 'staging_wake_status=scheduled\n'
printf 'staging_autostart_wired=%s\n' "$enablement"
printf 'production_service_final=active\n'
printf 'staging_service_final=active\n'
printf 'gaudere wake host-downtime arm: PASS\n'
