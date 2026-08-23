#!/bin/sh
set -eu

deploy=scripts/deploy-schema-v4.sh
workspace=$(mktemp -d)
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

cat > "$workspace/systemctl" <<'SH'
#!/bin/sh
set -eu
[ "$1" = "--user" ] || exit 90
[ "$2" = "is-active" ] || exit 91
printf 'inactive\n'
exit 3
SH
chmod +x "$workspace/systemctl"

cat > "$workspace/provenance.sh" <<'SH'
#!/bin/sh
set -eu
[ -f "$1" ]
[ -f "$1.sha256" ]
cat <<'OUT'
status=PREP_ONLY_NOT_AUTHORIZED_FOR_PRODUCTION
provider_effects=0
wake_effects=0
production_state_touched=false
gaudere schema v4 image provenance validation: PASS
OUT
SH
chmod +x "$workspace/provenance.sh"

cat > "$workspace/stage.sh" <<'SH'
#!/bin/sh
set -eu
state=${GAUDERE_STATE_DIR:?}
test ! -e "$state/state.db-wal"
test ! -e "$state/state.db-shm"
printf 'stage-called\n' > "${GAUDERE_TEST_STAGE_MARKER:?}"
SH
chmod +x "$workspace/stage.sh"

make_clean_wal_db()
{
    database=$1
    mkdir -p "$(dirname "$database")"
    python3 - "$database" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as db:
    mode = db.execute("PRAGMA journal_mode=WAL").fetchone()[0]
    assert mode.lower() == "wal"
    db.execute("CREATE TABLE durable(value TEXT)")
    db.execute("INSERT INTO durable VALUES ('safe')")
PY
}

create_readonly_sidecars()
{
    database=$1
    python3 - "$database" <<'PY'
import pathlib
import sqlite3
import sys
import urllib.parse

path = pathlib.Path(sys.argv[1])
uri = "file:" + urllib.parse.quote(str(path)) + "?mode=ro"
with sqlite3.connect(uri, uri=True) as db:
    assert db.execute("SELECT value FROM durable").fetchone()[0] == "safe"
PY
}

run_deploy()
{
    state=$1
    backups=$2
    marker=$3
    GAUDERE_TEST_MODE=1 \
    GAUDERE_STATE_DIR="$state" \
    GAUDERE_BACKUP_DIR="$backups" \
    GAUDERE_SERVICE_NAME=gaudere-agent.service \
    GAUDERE_SCHEMA_V4_IMAGE_PROVENANCE_VALIDATOR="$workspace/provenance.sh" \
    GAUDERE_SCHEMA_V4_STAGE_SCRIPT="$workspace/stage.sh" \
    GAUDERE_TEST_STAGE_MARKER="$marker" \
    SYSTEMCTL="$workspace/systemctl" \
        sh "$deploy"
}

# Reproduce the Fedora failure precisely: a clean WAL database has no sidecars after
# its writer closes, but a later ordinary read-only SQLite observation creates an
# empty -wal and a -shm file. The deployment gate may remove only this empty pair.
zero_root="$workspace/zero"
zero_state="$zero_root/state"
zero_backups="$zero_root/backups"
zero_marker="$zero_root/stage.marker"
make_clean_wal_db "$zero_state/state.db"
test ! -e "$zero_state/state.db-wal"
test ! -e "$zero_state/state.db-shm"
create_readonly_sidecars "$zero_state/state.db"
test -f "$zero_state/state.db-wal"
test ! -s "$zero_state/state.db-wal"
test -f "$zero_state/state.db-shm"

run_deploy "$zero_state" "$zero_backups" "$zero_marker" \
    > "$zero_root/out" 2> "$zero_root/err"
grep -qx 'sqlite_sidecar_fence=PASS' "$zero_root/out"
test ! -e "$zero_state/state.db-wal"
test ! -e "$zero_state/state.db-shm"
test -f "$zero_marker"
python3 - "$zero_state/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("SELECT value FROM durable").fetchone()[0] == "safe"
PY

# A non-empty WAL can contain committed frames that are not in the main database.
# It must never be pruned merely to satisfy the deployment fence.
nonempty_root="$workspace/nonempty"
nonempty_state="$nonempty_root/state"
nonempty_backups="$nonempty_root/backups"
nonempty_marker="$nonempty_root/stage.marker"
mkdir -p "$nonempty_state"
python3 - "$nonempty_state/state.db" <<'PY'
import os
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
assert db.execute("PRAGMA journal_mode=WAL").fetchone()[0].lower() == "wal"
db.execute("CREATE TABLE durable(value TEXT)")
db.execute("INSERT INTO durable VALUES ('committed-in-wal')")
db.commit()
# Deliberately bypass SQLite close/checkpoint to model an abnormal process exit.
os._exit(0)
PY

test -s "$nonempty_state/state.db-wal"
if run_deploy "$nonempty_state" "$nonempty_backups" "$nonempty_marker" \
        > "$nonempty_root/out" 2> "$nonempty_root/err"; then
    printf 'schema-v4 deployment unexpectedly discarded a non-empty WAL\n' >&2
    exit 1
fi
grep -q 'non-empty SQLite WAL remains in stopped state' "$nonempty_root/err"
grep -q 'stopped SQLite sidecar fence failed' "$nonempty_root/err"
test -s "$nonempty_state/state.db-wal"
test ! -e "$nonempty_marker"

printf 'gaudere schema v4 empty WAL sidecar test: PASS\n'
