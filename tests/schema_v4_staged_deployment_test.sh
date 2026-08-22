#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
deployer="$repository_root/scripts/deploy-schema-v4.sh"
agent_bin=${GAUDERE_AGENT_BIN:-}

if [ -z "$agent_bin" ]; then
    if [ -x "../src/gaudere-agent" ]; then
        agent_bin=../src/gaudere-agent
    elif [ -x "$repository_root/build/src/gaudere-agent" ]; then
        agent_bin="$repository_root/build/src/gaudere-agent"
    else
        printf 'schema-v4 staged deployment test: GAUDERE_AGENT_BIN is required\n' >&2
        exit 1
    fi
fi

[ -x "$agent_bin" ] || {
    printf 'schema-v4 staged deployment test: Agent binary is not executable\n' >&2
    exit 1
}
[ -f "$deployer" ] || {
    printf 'schema-v4 staged deployment test: deployment script not found\n' >&2
    exit 1
}

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-schema-v4-deploy.XXXXXX")
data_home="$workspace/data"
state_parent="$data_home/gaudere"
state="$state_parent/state"
backups="$state_parent/backups"
systemctl_log="$workspace/systemctl.log"
agent_log="$workspace/agent.log"
mkdir -p "$state" "$backups"

cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

fake_systemctl="$workspace/systemctl"
cat > "$fake_systemctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >> "$GAUDERE_TEST_SYSTEMCTL_LOG"
[ "$#" -eq 3 ] && [ "$1" = "--user" ] && [ "$2" = "is-active" ] \
    || { printf 'unexpected synthetic systemctl operation: %s\n' "$*" >&2; exit 90; }
printf '%s\n' "${GAUDERE_TEST_SERVICE_STATE:-inactive}"
SH
chmod +x "$fake_systemctl"

agent_wrapper="$workspace/agent-wrapper"
cat > "$agent_wrapper" <<'SH'
#!/bin/sh
state_path=
wake_enabled=0
previous=
for argument in "$@"; do
    if [ "$previous" = "--state" ]; then
        state_path=$argument
    fi
    [ "$argument" != "--wake-intents" ] || wake_enabled=1
    case "$argument" in
        --openai-model|--openai-secret|--secret-dir|--enqueue-openai|--openai-once)
            printf 'provider-capable argument reached synthetic migration: %s\n' \
                "$argument" >&2
            exit 93
            ;;
    esac
    previous=$argument
done
printf 'state=%s wake=%s args=%s\n' "$state_path" "$wake_enabled" "$*" \
    >> "$GAUDERE_TEST_AGENT_LOG"
if [ "$wake_enabled" = "1" ] && [ "$state_path" = "$GAUDERE_TEST_LIVE_DB" ]; then
    printf 'wake capability was enabled on the installed state\n' >&2
    exit 92
fi
if [ "${GAUDERE_TEST_FAIL_POST_SWAP:-0}" = "1" ] \
    && [ "$state_path" = "$GAUDERE_TEST_LIVE_DB" ]; then
    printf 'synthetic post-swap default-reopen failure\n' >&2
    exit 91
fi
exec "$GAUDERE_TEST_REAL_AGENT" "$@"
SH
chmod +x "$agent_wrapper"

"$agent_bin" --state "$state/state.db" \
    --echo historical-plain "durable plain result" >/dev/null

python3 - "$state/state.db" <<'PY'
import json
import sqlite3
import sys

usage = {
    "cache_write_input_tokens": 0,
    "cached_input_tokens": 0,
    "input_tokens": 166,
    "model": "gpt-5.6-sol",
    "output_tokens": 314,
    "provider": "openai",
    "reasoning_tokens": 43,
    "schema": "gaudere.provider_usage.v1",
    "total_tokens": 480,
}
usage_json = json.dumps(usage, sort_keys=True, separators=(",", ":"))
decision = json.dumps(
    {"decision": "stop", "schema": "gaudere.cognition.decision.v1"},
    sort_keys=True,
    separators=(",", ":"),
)

with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 3
    db.execute(
        "UPDATE tasks SET result_metadata_content_type=?,result_metadata=? "
        "WHERE id='historical-plain'",
        ("application/vnd.gaudere.provider-usage+json", usage_json),
    )

    columns = [row[1] for row in db.execute("PRAGMA table_info(tasks)")]
    source = db.execute(
        "SELECT * FROM tasks WHERE id='historical-plain'"
    ).fetchone()
    reflection = dict(zip(columns, source))
    reflection.update({
        "id": "historical-reflection",
        "idempotency_key": "cognition.reflect.v1:historical-reflection",
        "kind": "cognition.reflect.v1",
        "input_content_type": "application/json",
        "input": '{"objective":"historical"}',
        "result_content_type": "application/vnd.gaudere.cognition-decision+json",
        "result_output": decision,
        "result_metadata_content_type": "application/vnd.gaudere.provider-usage+json",
        "result_metadata": usage_json,
    })
    quoted_columns = ",".join('"' + column.replace('"', '""') + '"'
                              for column in columns)
    placeholders = ",".join("?" for _ in columns)
    db.execute(
        f"INSERT INTO tasks({quoted_columns}) VALUES({placeholders})",
        [reflection[column] for column in columns],
    )
    db.execute(
        "INSERT INTO actions(id,idempotency_key,critical,status,effect_result,"
        "lease_owner,lease_expires_at_ms) VALUES(?,?,?,?,?,?,?)",
        ("historical-action", "historical-action-key", 1, 3, 1, None, None),
    )
    db.executescript("""
        CREATE TABLE budget_consumptions (
          scope TEXT NOT NULL,
          idempotency_key TEXT NOT NULL,
          consumed_at_ms INTEGER NOT NULL,
          PRIMARY KEY(scope,idempotency_key)
        );
        CREATE INDEX idx_budget_consumptions_scope_time
          ON budget_consumptions(scope,consumed_at_ms);
    """)
    db.executemany(
        "INSERT INTO budget_consumptions VALUES(?,?,?)",
        [
            ("provider.call:openai.responses", "call-1", 1787341928220),
            ("provider.call:openai.responses", "call-2", 1787351325945),
            ("provider.call:openai.responses", "call-3", 1787397758294),
            ("wake.accept:cognition.reflect.wake.v0", "not-a-provider-call", 1787397758295),
        ],
    )
PY

sha256sum "$state/state.db" > "$workspace/source.before.sha256"
source_database_hash=$(sha256sum "$state/state.db")
source_database_hash=${source_database_hash%% *}

run_deployer()
{
    GAUDERE_AGENT_BIN="$agent_wrapper" \
    GAUDERE_TEST_MODE=1 \
    GAUDERE_TEST_REAL_AGENT="$agent_bin" \
    GAUDERE_TEST_LIVE_DB="$state/state.db" \
    GAUDERE_TEST_AGENT_LOG="$agent_log" \
    GAUDERE_TEST_SYSTEMCTL_LOG="$systemctl_log" \
    GAUDERE_STATE_DIR="$state" \
    GAUDERE_BACKUP_DIR="$backups" \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    SYSTEMCTL="$fake_systemctl" \
    sh "$deployer"
}

printf 'active\n' > "$workspace/service-state-marker"
if GAUDERE_TEST_SERVICE_STATE=active run_deployer \
        > "$workspace/active.out" 2>&1; then
    printf 'schema-v4 staged deployment accepted an active service\n' >&2
    exit 1
fi
grep -q 'must report exactly inactive (found active)' "$workspace/active.out"
[ -z "$(find "$backups" -maxdepth 1 -type f -print -quit)" ]

lock_marker="$workspace/lock-held"
(
    exec 9>>"$state/state.db.lock"
    flock 9
    : > "$lock_marker"
    sleep 2
) &
locker=$!
while [ ! -f "$lock_marker" ]; do sleep 0.02; done
if run_deployer > "$workspace/locked.out" 2>&1; then
    printf 'schema-v4 staged deployment accepted an owned state\n' >&2
    exit 1
fi
wait "$locker"
grep -q 'state database is currently owned' "$workspace/locked.out"
sha256sum "$state/state.db" > "$workspace/source.after-refusals.sha256"
cmp -s "$workspace/source.before.sha256" \
    "$workspace/source.after-refusals.sha256"

: > "$agent_log"
if GAUDERE_TEST_FAIL_POST_SWAP=1 run_deployer \
        > "$workspace/injected-failure.out" 2>&1; then
    printf 'schema-v4 staged deployment survived a post-swap failure\n' >&2
    exit 1
fi
grep -q 'synthetic post-swap default-reopen failure' \
    "$workspace/injected-failure.out"
grep -q '^automatic_rollback=STARTED$' "$workspace/injected-failure.out"
grep -q '^automatic_rollback=PASS$' "$workspace/injected-failure.out" || {
    cat "$workspace/injected-failure.out" >&2
    exit 1
}

sha256sum "$state/state.db" > "$workspace/source.after-rollback.sha256"
cmp -s "$workspace/source.before.sha256" \
    "$workspace/source.after-rollback.sha256"
python3 - "$state/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 3
    assert db.execute(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE name='wake_intents' OR tbl_name='wake_intents'"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM budget_consumptions "
        "WHERE scope='provider.call:openai.responses'"
    ).fetchone()[0] == 3
PY

[ -z "$(find "$state_parent" -maxdepth 1 -type d \
    -name 'state.pre-v4-*' -print -quit)" ]
failed=$(find "$state_parent" -maxdepth 1 -type d \
    -name 'state.failed-v4-*' -print -quit)
[ -n "$failed" ]
python3 - "$failed/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 4
    assert db.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0] == 0
PY
rm -rf "$failed"

while IFS= read -r invocation; do
    case "$invocation" in
        *"wake=1"*)
            case "$invocation" in
                *"state=$state/state.db"*)
                    printf 'wake-enabled invocation reached installed state\n' >&2
                    exit 1
                    ;;
                *"/.state.v4-work."*) ;;
                *)
                    printf 'wake-enabled invocation did not target staging: %s\n' \
                        "$invocation" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
done < "$agent_log"

: > "$agent_log"
run_deployer > "$workspace/success.out"

grep -q '^status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION$' \
    "$workspace/success.out"
grep -q '^schema_before=3$' "$workspace/success.out"
grep -q '^staged_schema_before=3$' "$workspace/success.out"
grep -q '^staged_schema_after=4$' "$workspace/success.out"
grep -q '^provider_budget_rows_before=3$' "$workspace/success.out"
grep -q '^provider_budget_rows_staged=3$' "$workspace/success.out"
grep -q '^provider_budget_rows_after=3$' "$workspace/success.out"
grep -q '^wake_rows=0$' "$workspace/success.out"
grep -q '^wake_armed=0$' "$workspace/success.out"
grep -q '^wake_fired=0$' "$workspace/success.out"
grep -q '^wake_revoked=0$' "$workspace/success.out"
grep -q '^wake_manual_review=0$' "$workspace/success.out"
grep -q '^schema_after=4$' "$workspace/success.out"
grep -q '^service_state=inactive$' "$workspace/success.out"
grep -q '^wake_capability_active=false$' "$workspace/success.out"
grep -q '^gaudere staged schema v4 deployment: PREPARED$' \
    "$workspace/success.out"
grep -q '^PREP ONLY / NOT AUTHORIZED FOR PRODUCTION; service remains stopped\.$' \
    "$workspace/success.out"

rollback=$(sed -n 's/^rollback_directory=//p' "$workspace/success.out" | tail -n 1)
[ -d "$rollback" ]
rollback_database_hash=$(sha256sum "$rollback/state.db")
rollback_database_hash=${rollback_database_hash%% *}
[ "$rollback_database_hash" = "$source_database_hash" ]
archive=$(sed -n 's/^backup=//p' "$workspace/success.out" | tail -n 1)
[ -f "$archive" ] && [ -f "$archive.sha256" ]
(
    cd "$backups"
    sha256sum -c "$(basename "$archive.sha256")"
)

python3 - "$rollback/state.db" "$state/state.db" <<'PY'
import base64
import json
import sqlite3
import sys

def encode(value):
    if isinstance(value, bytes):
        return {"bytes_base64": base64.b64encode(value).decode("ascii")}
    return value

def snapshot(path):
    with sqlite3.connect(path) as db:
        objects = db.execute(
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
            quoted = '"' + table.replace('"', '""') + '"'
            rows = [[encode(value) for value in row]
                    for row in db.execute(f"SELECT * FROM {quoted}").fetchall()]
            rows.sort(key=lambda row: json.dumps(
                row, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
            contents[table] = {
                "columns": db.execute(f"PRAGMA table_xinfo({quoted})").fetchall(),
                "rows": rows,
            }
        return {"objects": objects, "tables": contents}

with sqlite3.connect(sys.argv[1]) as before:
    assert before.execute("PRAGMA user_version").fetchone()[0] == 3
    assert before.execute(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE name='wake_intents' OR tbl_name='wake_intents'"
    ).fetchone()[0] == 0

with sqlite3.connect(sys.argv[2]) as after:
    assert after.execute("PRAGMA user_version").fetchone()[0] == 4
    assert after.execute("SELECT COUNT(*) FROM wake_intents").fetchone()[0] == 0
    metadata = after.execute(
        "SELECT id,result_metadata_content_type,result_metadata FROM tasks "
        "ORDER BY id"
    ).fetchall()
    assert len(metadata) == 2
    assert all(row[1] == "application/vnd.gaudere.provider-usage+json"
               for row in metadata)
    assert after.execute(
        "SELECT COUNT(*) FROM actions WHERE id='historical-action' "
        "AND status=3 AND effect_result=1"
    ).fetchone()[0] == 1
    assert after.execute(
        "SELECT COUNT(*) FROM budget_consumptions "
        "WHERE scope='provider.call:openai.responses'"
    ).fetchone()[0] == 3

assert snapshot(sys.argv[1]) == snapshot(sys.argv[2])
PY

while IFS= read -r invocation; do
    case "$invocation" in
        *"wake=1"*)
            case "$invocation" in
                *"state=$state/state.db"*)
                    printf 'wake-enabled invocation reached installed state\n' >&2
                    exit 1
                    ;;
                *"/.state.v4-work."*) ;;
                *)
                    printf 'wake-enabled invocation did not target staging: %s\n' \
                        "$invocation" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
done < "$agent_log"

if grep -Ev '^--user is-active gaudere-agent\.service$' "$systemctl_log" \
        | grep -q .; then
    printf 'deployment attempted a non-observational service operation\n' >&2
    exit 1
fi

printf 'gaudere schema v4 staged deployment tests: PASS\n'
