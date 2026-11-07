#!/bin/sh
set -eu

authorization=${GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_AUTHORIZATION:-}
expected_lineage_id=cognition.local-goose-dialogue.responder-retry-lineage.v1:da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca
expected_image=93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071
expected_core=1316cf68db93e4c91a7bd79fbd289b8f382f8659
consumer=system-responder-v1
event_id=cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844
result_sha=4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935
task_id=cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_dir="$data_home/gaudere/state"
state_db="$state_dir/state.db"
thread_db="$state_dir/local-goose-dialogue-thread.db"
completion_db="$state_dir/local-goose-dialogue-completion.db"
responder_db="$state_dir/local-goose-dialogue-responder.db"
retry_db="$state_dir/local-goose-dialogue-responder-retry.db"
backup_script="$script_directory/backup-state.sh"
service=gaudere-agent.service
workspace=""
backup=""
stopped=0

fail() { printf 'retry frozen snapshot: FAIL: %s\n' "$*" >&2; exit 1; }

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$stopped" = 1 ]; then
        systemctl --user start "$service" >/dev/null 2>&1 || true
        printf 'SERVICE_RECOVERY=restart_attempted_without_production_commit\n' >&2
    fi
    if [ "$rc" -ne 0 ]; then
        printf 'FEDORA_GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_ISOLATED=ABORTED\n' >&2
        [ -z "$backup" ] || printf 'BACKUP=%s\n' "$backup" >&2
        [ -z "$workspace" ] || printf 'TRANSITION_WORKSPACE=%s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

require_line()
{
    printf '%s\n' "$1" | grep -Fxq "$2" || fail "missing exact proof line: $2"
}
image_id() { podman inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://'; }
network_mode() { podman inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null; }
provider_total()
{
    sqlite3 -readonly "$state_db" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';"
}
verify_trigger()
{
    cursor=$(sqlite3 -readonly "$completion_db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer';") || fail "cursor unreadable"
    [ "$cursor" = 3 ] || fail "responder cursor differs"
    row=$(sqlite3 -readonly "$completion_db" "SELECT event_id||'|'||result_sha256||'|'||thread_alias||'|'||thread_revision||'|'||task_id||'|'||speaker_kind||'|'||speaker_id FROM local_goose_dialogue_completion_event WHERE sequence=4;") || fail "event unreadable"
    [ "$row" = "$event_id|$result_sha|main|3|$task_id|human|bertrand" ] || fail "sequence 4 differs"
    head=$(sqlite3 -readonly "$thread_db" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='main' AND revision=3 AND head_task_id='$task_id';") || fail "head unreadable"
    [ "$head" = 1 ] || fail "preferred head differs"
}
verify_live()
{
    [ "$(systemctl --user is-active "$service" 2>/dev/null)" = active ] || fail "production service is not active"
    [ "$(image_id)" = "$expected_image" ] || fail "production image differs"
    [ "$(network_mode)" = none ] || fail "production network differs"
    [ "$(provider_total)" = 10 ] || fail "provider total differs"
    [ "$(sqlite3 -readonly "$responder_db" 'PRAGMA user_version;')" = 3 ] || fail "authority schema differs"
    [ -f "$retry_db" ] && [ ! -L "$retry_db" ] || fail "production retry sidecar missing or unsafe"
    [ "$(stat -c %a "$retry_db")" = 600 ] || fail "retry sidecar mode differs"
    [ "$(sqlite3 -readonly "$retry_db" 'PRAGMA user_version;')" = 1 ] || fail "retry schema differs"
    [ "$(sqlite3 -readonly "$retry_db" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage;')" = 1 ] || fail "retry row count differs"
    [ "$(sqlite3 -readonly "$retry_db" "SELECT lineage_id FROM local_goose_dialogue_responder_retry_lineage WHERE consumer_id='$consumer' AND completion_sequence=4 AND event_id='$event_id' AND retry_generation=1;")" = "$expected_lineage_id" ] || fail "retry lineage identity differs"
    for suffix in -wal -shm -journal; do
        [ ! -e "$retry_db$suffix" ] && [ ! -L "$retry_db$suffix" ] || fail "retry sidecar has SQLite companion"
    done
    verify_trigger

    observer=$(podman exec gaudere-agent \
        /usr/local/bin/gaudere-local-goose-dialogue-responder-observer status \
        --thread-sidecar /var/lib/gaudere/local-goose-dialogue-thread.db \
        --completion-sidecar /var/lib/gaudere/local-goose-dialogue-completion.db \
        --responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db) || fail "observer unavailable"
    for expected in classification=intervention_owned observer_armed=no \
        consumer_last_sequence=3 next_sequence=4 \
        "next_event_id=$event_id" "next_result_sha256=$result_sha" \
        head_revision=3 disposition_kind=3 disposition_state=4
    do
        require_line "$observer" "$expected"
    done
    capability=$(podman exec gaudere-agent \
        /usr/local/bin/gaudere-local-goose-dialogue-responder-authority-capability --check) || fail "capability unavailable"
    for expected in AUTHORITY_DEFAULT=disabled STATE_ACCESS=absent MUTATION_SURFACE=absent \
        GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_CAPABILITY=PASS
    do
        require_line "$capability" "$expected"
    done
}
logical_sha()
{
    sqlite3 -readonly "$1" '.dump' > "$workspace/sqlite-dump.tmp" || fail "cannot dump SQLite: $1"
    sha256sum "$workspace/sqlite-dump.tmp" | awk '{print $1}'
}
verify_production_hashes()
{
    [ "$(logical_sha "$state_db")" = "$state_sha" ] || fail "production Task/provider state changed"
    [ "$(logical_sha "$thread_db")" = "$thread_sha" ] || fail "production thread state changed"
    [ "$(logical_sha "$completion_db")" = "$completion_sha" ] || fail "production completion/ACK state changed"
    [ "$(logical_sha "$responder_db")" = "$responder_sha" ] || fail "production historical authority state changed"
}
helper()
{
    podman run --rm --network none --userns keep-id --user "$uid:$gid" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --security-opt label=disable --cap-drop all \
        -v "$rehearsal:/rehearsal:rw" "$helper_image" "$1"
}

[ "$authorization" = AUTHORIZED_RESPONDER_RETRY_FROZEN_SNAPSHOT ] || fail "explicit frozen snapshot gate authorization required"
for cmd in git podman systemctl sqlite3 python3 sha256sum awk sed grep tr id mkdir mktemp sh sleep stat; do
    command -v "$cmd" >/dev/null 2>&1 || fail "missing command: $cmd"
done
for f in "$backup_script" "$repo/Containerfile" "$repo/Containerfile.responder-retry-frozen-snapshot" \
    "$state_db" "$thread_db" "$completion_db" "$responder_db" "$retry_db"; do
    [ -f "$f" ] && [ ! -L "$f" ] || fail "file missing or symlink: $f"
done
[ "$(podman info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = true ] || fail "rootless Podman required"
[ "$(git -C "$repo" rev-parse --show-toplevel)" = "$repo" ] || fail "unexpected checkout"
[ "$(git -C "$repo" branch --show-current)" = main ] || fail "main required"
[ -z "$(git -C "$repo" status --porcelain --untracked-files=normal)" ] || fail "checkout must be clean"
git -C "$repo" fetch --quiet origin main
agent_ref=$(git -C "$repo" rev-parse HEAD)
[ "$agent_ref" = "$(git -C "$repo" rev-parse origin/main)" ] || fail "not current origin/main"
case "$agent_ref" in *[!0-9a-f]*|'') fail "invalid agent ref" ;; esac
[ "${#agent_ref}" -eq 40 ] || fail "agent ref length differs"
core_ref=$(tr -d '\r\n' < "$repo/gaudere.ref")
[ "$core_ref" = "$expected_core" ] || fail "core ref differs"
uid=$(id -u)
gid=$(id -g)

printf '=== CHECK PRODUCTION PRESTATE ===\n'
verify_live

printf '=== BUILD CONFINED HELPER ===\n'
builder_image="localhost/gaudere-agent:retry-frozen-snapshot-builder-$agent_ref"
helper_image="localhost/gaudere-agent:retry-frozen-snapshot-inspect-$agent_ref"
podman build --target builder --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --build-arg "GAUDERE_REF=$core_ref" --tag "$builder_image" \
    --file "$repo/Containerfile" "$repo"
podman build --build-arg "BUILDER_IMAGE=$builder_image" \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" --build-arg "GAUDERE_REF=$core_ref" \
    --tag "$helper_image" --file "$repo/Containerfile.responder-retry-frozen-snapshot" "$repo"
[ "$(podman image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.agent.revision"}}')" = "$agent_ref" ] || fail "helper agent provenance differs"
[ "$(podman image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.core.revision"}}')" = "$core_ref" ] || fail "helper core provenance differs"
[ "$(podman image inspect "$helper_image" --format '{{index .Config.Labels "io.gaudere.role"}}')" = dialogue-responder-retry-frozen-snapshot-inspect-v1 ] || fail "helper role differs"
help=$(podman run --rm --network none --read-only --read-only-tmpfs \
    --security-opt no-new-privileges --cap-drop all "$helper_image" --help) || fail "helper help failed"
require_line "$help" "Usage: retry-frozen-snapshot <inspect-generation-1> (fixed /rehearsal paths only)"

printf '=== STOP SERVICE AND CAPTURE FIVE FROZEN DATABASES ===\n'
systemctl --user stop "$service" || fail "cannot stop production service"
stopped=1
[ "$(systemctl --user is-active "$service" 2>/dev/null || true)" != active ] || fail "service did not stop"
backup=$(GAUDERE_STATE_DIR="$state_dir" sh "$backup_script") || fail "stopped-state backup failed"
[ -f "$backup" ] && [ ! -L "$backup" ] || fail "backup unsafe"
printf 'BACKUP=%s\n' "$backup"

root="$data_home/gaudere/.responder-retry-frozen-snapshot-proof"
mkdir -p -m 0700 "$root"
workspace=$(mktemp -d "$root/transition.XXXXXX")
rehearsal="$workspace/rehearsal"
mkdir -p -m 0700 "$rehearsal"
retry_raw_sha=$(sha256sum "$retry_db" | awk '{print $1}')
python3 - "$rehearsal" "$state_db" "$thread_db" "$completion_db" "$responder_db" "$retry_db" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys
destination = Path(sys.argv[1])
for arg in sys.argv[2:]:
    source = Path(arg).resolve()
    target = destination / source.name
    reader = sqlite3.connect(source.as_uri() + "?mode=ro", uri=True)
    try:
        writer = sqlite3.connect(target)
        try:
            reader.backup(writer)
        finally:
            writer.close()
    finally:
        reader.close()
    os.chmod(target, 0o600)
PY

state_sha=$(logical_sha "$rehearsal/state.db")
thread_sha=$(logical_sha "$rehearsal/local-goose-dialogue-thread.db")
completion_sha=$(logical_sha "$rehearsal/local-goose-dialogue-completion.db")
responder_sha=$(logical_sha "$rehearsal/local-goose-dialogue-responder.db")
retry_sha=$(logical_sha "$rehearsal/local-goose-dialogue-responder-retry.db")
[ "$(logical_sha "$retry_db")" = "$retry_sha" ] || fail "retry copy differs from stopped production"

printf '=== RESTART UNCHANGED SERVICE BEFORE INSPECTION ===\n'
systemctl --user start "$service" || fail "service restart failed"
stopped=0
sleep 3
verify_live
verify_production_hashes
[ "$(logical_sha "$retry_db")" = "$retry_sha" ] || fail "production retry audit changed"
[ "$(sha256sum "$retry_db" | awk '{print $1}')" = "$retry_raw_sha" ] || fail "production retry bytes changed"

printf '=== INSPECT GENERATION-1 RETRY ON COPY ONLY ===\n'
[ -f "$rehearsal/local-goose-dialogue-responder-retry.db" ] || fail "retry copy missing"
[ "$(stat -c %a "$rehearsal/local-goose-dialogue-responder-retry.db")" = 600 ] || fail "retry copy permissions differ"
for suffix in -wal -shm -journal; do
    [ ! -e "$rehearsal/local-goose-dialogue-responder-retry.db$suffix" ] || fail "retry copy has SQLite companion"
done

inspection=$(helper inspect-generation-1) || fail "frozen copy inspection rejected"
printf '%s\n' "$inspection"
for expected in RETRY_SIDECAR_READ_ONLY=yes RETRY_GENERATION=1 \
    "LINEAGE_ID=$expected_lineage_id" ELIGIBILITY=evidence_consistent_no_authority \
    RETRY_AUTHORITY=absent GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_INSPECT=PASS
do
    require_line "$inspection" "$expected"
done

printf '=== CHECK COPY HASHES AND LIVE PRODUCTION ===\n'
[ "$(logical_sha "$rehearsal/state.db")" = "$state_sha" ] || fail "Task/provider copy changed"
[ "$(logical_sha "$rehearsal/local-goose-dialogue-thread.db")" = "$thread_sha" ] || fail "thread copy changed"
[ "$(logical_sha "$rehearsal/local-goose-dialogue-completion.db")" = "$completion_sha" ] || fail "completion copy changed"
[ "$(logical_sha "$rehearsal/local-goose-dialogue-responder.db")" = "$responder_sha" ] || fail "authority copy changed"
[ "$(logical_sha "$rehearsal/local-goose-dialogue-responder-retry.db")" = "$retry_sha" ] || fail "retry audit copy changed"
for suffix in -wal -shm -journal; do
    [ ! -e "$rehearsal/local-goose-dialogue-responder-retry.db$suffix" ] || fail "retry reader created SQLite companion"
done
verify_live
verify_production_hashes
[ "$(logical_sha "$retry_db")" = "$retry_sha" ] || fail "production retry audit changed"
[ "$(sha256sum "$retry_db" | awk '{print $1}')" = "$retry_raw_sha" ] || fail "production retry bytes changed"

printf 'PRODUCTION_FIVE_SQLITE_LOGICAL_HASHES=unchanged\n'
printf 'PRODUCTION_RETRY_SIDECAR=present\n'
printf 'PRODUCTION_RETRY_LINEAGE_ID=%s\n' "$expected_lineage_id"
printf 'PRODUCTION_RETRY_BYTES=unchanged\n'
printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$(image_id)"
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'BACKUP=%s\n' "$backup"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'SEQUENCE4_ACK=no\n'
printf 'SUBMISSION_TASK=absent\n'
printf 'FEDORA_GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_ISOLATED=PASS\n'
trap - EXIT HUP INT TERM
