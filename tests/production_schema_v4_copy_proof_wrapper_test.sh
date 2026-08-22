#!/bin/sh
set -eu

srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
wrapper="$srcdir/scripts/validate-production-schema-v4-copy-proof.sh"
workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-v4-host-proof-test.XXXXXX")
state="$workspace/state"
backups="$workspace/backups"
bin="$workspace/bin"
service_state="$workspace/service-state"
mkdir -p "$state" "$backups" "$bin"
trap 'rm -rf "$workspace"' EXIT HUP INT TERM
printf 'active\n' > "$service_state"

agent_ref=$(git -C "$srcdir" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$srcdir/gaudere.ref")
rollback_id="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
candidate_id="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

python3 - "$state/state.db" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as db:
    db.executescript("""
    PRAGMA user_version=3;
    CREATE TABLE budget_consumptions (
      scope TEXT NOT NULL,
      idempotency_key TEXT NOT NULL,
      consumed_at_ms INTEGER NOT NULL,
      PRIMARY KEY(scope,idempotency_key)
    );
    INSERT INTO budget_consumptions VALUES
      ('provider.call:openai.responses','one',1),
      ('provider.call:openai.responses','two',2),
      ('provider.call:openai.responses','three',3);
    """)
PY

cat > "$bin/systemctl" <<'SH'
#!/bin/sh
set -eu
state=${GAUDERE_TEST_SERVICE_STATE:?}
[ "$1" = "--user" ] || exit 90
case "$2" in
  is-active) cat "$state" ;;
  stop) printf 'inactive\n' > "$state" ;;
  start) printf 'active\n' > "$state" ;;
  *) exit 91 ;;
esac
SH

cat > "$bin/podman" <<'SH'
#!/bin/sh
set -eu
[ "$1" = "image" ] && [ "$2" = "inspect" ] || exit 92
printf '%s\n' "${GAUDERE_TEST_CANDIDATE_ID:?}"
SH

cat > "$workspace/control.sh" <<'SH'
#!/bin/sh
set -eu
case "$1" in
  budget)
    cat <<'EOF'
scope="provider.call:openai.responses"
provider_enabled=true
max_total=12
total_used=3
remaining_total=9
max_window=4
in_window_used=3
remaining_window=1
min_interval_seconds=900
last_consumed_at_ms=3
next_new_call=available
EOF
    ;;
  task)
    cat <<'EOF'
id="production-initiative-first"
kind="provider.openai.responses"
status=succeeded
attempts=1/2
result_content_type="text/plain; charset=utf-8"
result_output="synthetic historical task"
EOF
    ;;
  *) exit 93 ;;
esac
SH

cat > "$workspace/capture.sh" <<'SH'
#!/bin/sh
set -eu
manifest=${GAUDERE_ROLLBACK_MANIFEST:?}
mkdir -p "$(dirname "$manifest")"
printf 'rollback_image_id=%s\n' "${GAUDERE_TEST_ROLLBACK_ID:?}" > "$manifest"
printf 'rollback_capture=PASS\n'
SH

cat > "$workspace/build.sh" <<'SH'
#!/bin/sh
set -eu
printf 'synthetic_candidate_build=PASS\n'
SH

cat > "$workspace/backup.sh" <<'SH'
#!/bin/sh
set -eu
backup_dir=${GAUDERE_BACKUP_DIR:?}
mkdir -p "$backup_dir"
archive="$backup_dir/synthetic-state.tar.gz"
printf 'synthetic immutable backup\n' > "$archive"
(
  cd "$backup_dir"
  sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256"
)
printf '%s\n' "$archive"
SH

cat > "$workspace/proof-ok.sh" <<'SH'
#!/bin/sh
set -eu
[ "$GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS" = "3" ]
[ "$GAUDERE_EXPECTED_CANDIDATE_ID" = "$GAUDERE_TEST_CANDIDATE_ID" ]
[ "$GAUDERE_EXPECTED_ROLLBACK_ID" = "$GAUDERE_TEST_ROLLBACK_ID" ]
printf 'schema_before=3\n'
printf 'schema_after=4\n'
printf 'wake_rows=0\n'
printf 'provider_budget_rows_after=3\n'
printf 'production_state_touched=false\n'
printf 'gaudere schema v4 image provenance validation: PASS\n'
SH

cat > "$workspace/proof-fail.sh" <<'SH'
#!/bin/sh
printf 'synthetic injected proof failure\n' >&2
exit 42
SH

chmod +x "$bin/systemctl" "$bin/podman" \
    "$workspace/control.sh" "$workspace/capture.sh" "$workspace/build.sh" \
    "$workspace/backup.sh" "$workspace/proof-ok.sh" "$workspace/proof-fail.sh"

run_wrapper()
{
    proof_script=$1
    proof_id=$2
    GAUDERE_TEST_MODE=1 \
    GAUDERE_EXPECTED_AGENT_REF="$agent_ref" \
    GAUDERE_EXPECTED_CORE_REF="$core_ref" \
    GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS=3 \
    GAUDERE_REPRESENTATIVE_TASK=production-initiative-first \
    GAUDERE_PROOF_ID="$proof_id" \
    GAUDERE_STATE_DIR="$state" \
    GAUDERE_BACKUP_DIR="$backups" \
    GAUDERE_CONTROL_SCRIPT="$workspace/control.sh" \
    GAUDERE_CAPTURE_ROLLBACK_SCRIPT="$workspace/capture.sh" \
    GAUDERE_BUILD_IMAGE_SCRIPT="$workspace/build.sh" \
    GAUDERE_BACKUP_SCRIPT="$workspace/backup.sh" \
    GAUDERE_IMAGE_PROOF_SCRIPT="$proof_script" \
    GAUDERE_TEST_SERVICE_STATE="$service_state" \
    GAUDERE_TEST_CANDIDATE_ID="$candidate_id" \
    GAUDERE_TEST_ROLLBACK_ID="$rollback_id" \
    SYSTEMCTL="$bin/systemctl" PODMAN="$bin/podman" \
        sh "$wrapper"
}

before_hash=$(sha256sum "$state/state.db")
run_wrapper "$workspace/proof-ok.sh" success > "$workspace/success.out"
grep -q '^REAL_STATE_BYTE_IDENTITY=PASS$' "$workspace/success.out"
grep -q '^REAL_SCHEMA_AFTER_PROOF=3$' "$workspace/success.out"
grep -q '^REAL_WAKE_OBJECTS_AFTER_PROOF=0$' "$workspace/success.out"
grep -q '^REAL_PROVIDER_BUDGET_ROWS_AFTER_PROOF=3$' "$workspace/success.out"
grep -q '^REAL_SERVICE_AFTER_PROOF=active$' "$workspace/success.out"
grep -q '^gaudere production schema-v4 copy proof: PASS$' "$workspace/success.out"
[ "$(cat "$service_state")" = "active" ]
[ "$(sha256sum "$state/state.db")" = "$before_hash" ]

printf 'active\n' > "$service_state"
if run_wrapper "$workspace/proof-fail.sh" injected > "$workspace/fail.out" 2>&1; then
    echo "wrapper unexpectedly survived injected provenance failure" >&2
    exit 1
fi
[ "$(cat "$service_state")" = "active" ] \
    || { echo "service restart trap did not restore active state" >&2; exit 1; }
[ "$(sha256sum "$state/state.db")" = "$before_hash" ] \
    || { echo "synthetic production state changed after failure" >&2; exit 1; }

if GAUDERE_TEST_MODE=0 \
    GAUDERE_EXPECTED_AGENT_REF="$agent_ref" \
    GAUDERE_EXPECTED_CORE_REF="$core_ref" \
    GAUDERE_STATE_DIR="$state" GAUDERE_BACKUP_DIR="$backups" \
    GAUDERE_CONTROL_SCRIPT="$workspace/control.sh" \
    SYSTEMCTL="$bin/systemctl" PODMAN="$bin/podman" \
    sh "$wrapper" > "$workspace/override.out" 2>&1; then
    echo "production mode unexpectedly accepted a helper override" >&2
    exit 1
fi
grep -q 'override is restricted to synthetic test mode' "$workspace/override.out"

printf 'gaudere production schema-v4 copy proof wrapper test: PASS\n'
