#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backup_script="$script_directory/backup-state.sh"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/backup.XXXXXX")
source_state="$workspace/source-state"
restored_state="$workspace/restored-state"
backup_directory="$workspace/backups"
container_name="gaudere-backup-validation-$$"
mkdir -p "$source_state" "$restored_state" "$backup_directory"

cleanup()
{
    "$podman_command" rm -f "$container_name" >/dev/null 2>&1 || true
    if [ "${KEEP_GAUDERE_VALIDATION_STATE:-0}" = "1" ]; then
        printf 'backup validation state kept at %s\n' "$workspace" >&2
    else
        rm -rf "$workspace"
    fi
}
trap cleanup EXIT HUP INT TERM

fail()
{
    printf 'backup validation failure: %s\n' "$*" >&2
    exit 1
}

expect_line()
{
    text=$1
    pattern=$2
    if ! printf '%s\n' "$text" | grep -q "$pattern"; then
        printf 'backup validation failure: expected pattern %s\n' "$pattern" >&2
        printf '%s\n' "$text" >&2
        exit 1
    fi
}

run_state()
{
    directory=$1
    shift
    "$podman_command" run --rm \
        --network none \
        --userns keep-id \
        --read-only \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$directory:/var/lib/gaudere:Z" \
        "$image" \
        --state /var/lib/gaudere/state.db "$@"
}

start_source_service()
{
    "$podman_command" run -d \
        --name "$container_name" \
        --network none \
        --userns keep-id \
        --read-only \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$source_state:/var/lib/gaudere:Z" \
        "$image" \
        --state /var/lib/gaudere/state.db >/dev/null
}

for command in "$podman_command" tar sha256sum; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
[ -f "$backup_script" ] || fail "backup script not found: $backup_script"
"$podman_command" image exists "$image" \
    || fail "image does not exist: $image"

printf '\n==> create durable source state\n'
source_output=$(run_state "$source_state" --echo validation-backup "durable before backup")
expect_line "$source_output" 'gaudere-agent: echo result: durable before backup'

printf '\n==> prove live ownership blocks backup\n'
start_source_service
sleep 0.3
if GAUDERE_STATE_DIR="$source_state" GAUDERE_BACKUP_DIR="$backup_directory" \
    sh "$backup_script" >"$workspace/live-backup.out" 2>"$workspace/live-backup.err"; then
    fail "backup unexpectedly succeeded while gaudere-agent owned the state"
fi
if ! grep -q 'state database is currently owned' "$workspace/live-backup.err"; then
    cat "$workspace/live-backup.err" >&2
    fail "live backup refusal did not report ownership"
fi
"$podman_command" stop --time 5 "$container_name" >/dev/null
"$podman_command" rm "$container_name" >/dev/null

printf '\n==> create stopped-state archive and checksum\n'
archive=$(GAUDERE_STATE_DIR="$source_state" GAUDERE_BACKUP_DIR="$backup_directory" \
    sh "$backup_script")
[ -f "$archive" ] || fail "backup archive was not created"
[ -f "$archive.sha256" ] || fail "backup checksum was not created"
(
    cd "$backup_directory"
    sha256sum -c "$(basename "$archive.sha256")" >/dev/null
)

printf '\n==> restore into a fresh directory\n'
tar -xzf "$archive" -C "$restored_state"
[ -f "$restored_state/state.db" ] || fail "restored state.db is missing"
[ ! -e "$restored_state/state.db.lock" ] \
    || fail "coordination-only lock file should not be restored"

restored_report=$(run_state "$restored_state" --task validation-backup)
expect_line "$restored_report" '^status=succeeded$'
expect_line "$restored_report" '^result_output="durable before backup"$'

after_restore=$(run_state "$restored_state" --echo validation-after-restore "writable after restore")
expect_line "$after_restore" 'gaudere-agent: echo result: writable after restore'
expect_line "$after_restore" 'gaudere-agent: safe'

printf '\n==> backup/restore validation complete\n'
printf 'gaudere state backup validation: PASS\n'
