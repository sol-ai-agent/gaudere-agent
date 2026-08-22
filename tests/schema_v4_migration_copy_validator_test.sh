#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
validator="$repository_root/scripts/validate-schema-v4-migration-copy.sh"
backup_script="$repository_root/scripts/backup-state.sh"
agent_bin=${GAUDERE_AGENT_BIN:-}

if [ -z "$agent_bin" ]; then
    if [ -x "../src/gaudere-agent" ]; then
        agent_bin=../src/gaudere-agent
    elif [ -x "$repository_root/build/src/gaudere-agent" ]; then
        agent_bin="$repository_root/build/src/gaudere-agent"
    else
        printf 'schema-v4 validator test: GAUDERE_AGENT_BIN is required\n' >&2
        exit 1
    fi
fi

[ -x "$agent_bin" ] || { printf 'Agent binary is not executable\n' >&2; exit 1; }
[ -f "$validator" ] || { printf 'validator not found\n' >&2; exit 1; }
[ -f "$backup_script" ] || { printf 'backup script not found\n' >&2; exit 1; }

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-schema-v4-validator.XXXXXX")
source_state="$workspace/source-state"
backups="$workspace/backups"
data_home="$workspace/data"
mkdir -p "$source_state" "$backups" "$data_home"

cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

write_checksum()
{
    target=$1
    (
        cd "$(dirname "$target")"
        sha256sum "$(basename "$target")" \
            > "$(basename "$target").sha256"
    )
}

"$agent_bin" --state "$source_state/state.db" \
    --echo migration-proof-task "durable schema-v3 task" >/dev/null

python3 - "$source_state/state.db" <<'PY'
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
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 3
    db.execute(
        "UPDATE tasks SET result_metadata_content_type=?,result_metadata=? "
        "WHERE id='migration-proof-task'",
        ("application/vnd.gaudere.provider-usage+json",
         json.dumps(usage, sort_keys=True, separators=(",", ":"))),
    )
    db.execute(
        "INSERT INTO actions(id,idempotency_key,critical,status,effect_result,"
        "lease_owner,lease_expires_at_ms) VALUES(?,?,?,?,?,?,?)",
        ("migration-action", "migration-action-key", 1, 3, 1, None, None),
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
        ],
    )
PY

archive=$(GAUDERE_STATE_DIR="$source_state" \
    GAUDERE_BACKUP_DIR="$backups" sh "$backup_script")
[ -f "$archive" ] && [ -f "$archive.sha256" ]
sha256sum "$archive" > "$workspace/archive.before"

GAUDERE_AGENT_BIN="$agent_bin" \
GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS=3 \
XDG_DATA_HOME="$data_home" \
sh "$validator" "$archive" migration-proof-task > "$workspace/success.out"

grep -q '^schema_before=3$' "$workspace/success.out"
grep -q '^lock_refusal=PASS$' "$workspace/success.out"
grep -q '^schema_after=4$' "$workspace/success.out"
grep -q '^provider_budget_rows_before=3$' "$workspace/success.out"
grep -q '^provider_budget_rows_after=3$' "$workspace/success.out"
grep -q '^wake_rows=0$' "$workspace/success.out"
grep -q '^v4_reopen=PASS$' "$workspace/success.out"
grep -q '^rollback_schema_before=3$' "$workspace/success.out"
grep -q '^rollback_restore=PASS$' "$workspace/success.out"
grep -q '^archive_immutable=PASS$' "$workspace/success.out"
grep -q '^result_metadata_content_type="application/vnd.gaudere.provider-usage+json"$' \
    "$workspace/success.out"
grep -q '^gaudere schema v4 migration copy validation: PASS$' \
    "$workspace/success.out"

sha256sum "$archive" > "$workspace/archive.after"
cmp -s "$workspace/archive.before" "$workspace/archive.after"
python3 - "$source_state/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 3
    assert db.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE name='wake_intents'"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM budget_consumptions "
        "WHERE scope='provider.call:openai.responses'"
    ).fetchone()[0] == 3
PY

if GAUDERE_AGENT_BIN="$agent_bin" \
    GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS=4 \
    XDG_DATA_HOME="$data_home" \
    sh "$validator" "$archive" migration-proof-task \
    >"$workspace/wrong-budget.out" 2>&1; then
    printf 'validator accepted an incorrect provider budget expectation\n' >&2
    exit 1
fi
grep -q 'expected 4 provider budget rows, found 3' \
    "$workspace/wrong-budget.out"

missing_checksum="$workspace/no-checksum.tar.gz"
cp "$archive" "$missing_checksum"
if GAUDERE_AGENT_BIN="$agent_bin" XDG_DATA_HOME="$data_home" \
    sh "$validator" "$missing_checksum" \
    >"$workspace/no-checksum.out" 2>&1; then
    printf 'validator accepted an archive without a checksum\n' >&2
    exit 1
fi
grep -q 'verified backup checksum is required' "$workspace/no-checksum.out"

locked_state="$workspace/locked-state"
mkdir -p "$locked_state"
cp "$source_state/state.db" "$locked_state/state.db"
printf 'coordination only\n' > "$locked_state/state.db.lock"
locked_archive="$workspace/locked.tar.gz"
tar -czf "$locked_archive" -C "$locked_state" .
write_checksum "$locked_archive"
if GAUDERE_AGENT_BIN="$agent_bin" XDG_DATA_HOME="$data_home" \
    sh "$validator" "$locked_archive" \
    >"$workspace/locked-archive.out" 2>&1; then
    printf 'validator accepted an archive containing state.db.lock\n' >&2
    exit 1
fi
grep -q 'backup contains coordination-only state.db.lock' \
    "$workspace/locked-archive.out"

python3 - "$source_state/state.db" \
    "$workspace/unsafe.tar.gz" "$workspace/link.tar.gz" <<'PY'
import io
import pathlib
import sys
import tarfile

database = pathlib.Path(sys.argv[1]).read_bytes()

def add_file(archive, name, content):
    member = tarfile.TarInfo(name)
    member.size = len(content)
    archive.addfile(member, io.BytesIO(content))

with tarfile.open(sys.argv[2], "w:gz") as archive:
    add_file(archive, "state.db", database)
    add_file(archive, "../escape", b"escape")

with tarfile.open(sys.argv[3], "w:gz") as archive:
    add_file(archive, "state.db", database)
    link = tarfile.TarInfo("state-link")
    link.type = tarfile.SYMTYPE
    link.linkname = "state.db"
    archive.addfile(link)
PY

unsafe_archive="$workspace/unsafe.tar.gz"
write_checksum "$unsafe_archive"
if GAUDERE_AGENT_BIN="$agent_bin" XDG_DATA_HOME="$data_home" \
    sh "$validator" "$unsafe_archive" \
    >"$workspace/unsafe.out" 2>&1; then
    printf 'validator accepted an unsafe archive path\n' >&2
    exit 1
fi
grep -q 'backup contains an unsafe path' "$workspace/unsafe.out"

link_archive="$workspace/link.tar.gz"
write_checksum "$link_archive"
if GAUDERE_AGENT_BIN="$agent_bin" XDG_DATA_HOME="$data_home" \
    sh "$validator" "$link_archive" \
    >"$workspace/link.out" 2>&1; then
    printf 'validator accepted an archive link\n' >&2
    exit 1
fi
grep -q 'backup contains a link or unsupported file type' \
    "$workspace/link.out"

extra_manifest_archive="$workspace/extra-manifest.tar.gz"
cp "$archive" "$extra_manifest_archive"
write_checksum "$extra_manifest_archive"
sha256sum "$source_state/state.db" \
    >> "$extra_manifest_archive.sha256"
if GAUDERE_AGENT_BIN="$agent_bin" XDG_DATA_HOME="$data_home" \
    sh "$validator" "$extra_manifest_archive" \
    >"$workspace/extra-manifest.out" 2>&1; then
    printf 'validator accepted an ambiguous checksum manifest\n' >&2
    exit 1
fi
grep -q 'checksum manifest must contain exactly one entry' \
    "$workspace/extra-manifest.out"

printf 'gaudere schema v4 migration copy validator tests: PASS\n'
