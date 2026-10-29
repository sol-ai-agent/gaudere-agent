#!/bin/sh
set -eu

data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
backup_directory=${GAUDERE_BACKUP_DIR:-"$data_home/gaudere/backups"}
state_database="$state_directory/state.db"
lock_file="$state_database.lock"

fail()
{
    printf 'gaudere state backup: %s\n' "$*" >&2
    exit 1
}

for command in flock tar sha256sum date mktemp realpath; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done

[ -d "$state_directory" ] \
    || fail "state directory does not exist: $state_directory"
[ -f "$state_database" ] \
    || fail "state database does not exist: $state_database"

mkdir -p "$backup_directory"
state_absolute=$(realpath -m "$state_directory")
backup_absolute=$(realpath -m "$backup_directory")
case "$backup_absolute/" in
    "$state_absolute/"*)
        fail "backup directory must not be inside the state directory"
        ;;
esac

# Use the same advisory flock as gaudere-agent's StateLock. Holding this descriptor
# for the complete archive creation guarantees no gaudere-agent process owns the DB.
# The lock file itself is coordination only and is deliberately excluded from backup.
exec 9>>"$lock_file"
chmod 600 "$lock_file" 2>/dev/null || true
if ! flock -n 9; then
    printf 'gaudere state backup: state database is currently owned; stop gaudere-agent first\n' >&2
    exit 2
fi

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
archive="$backup_directory/gaudere-state-$timestamp-$$.tar.gz"
temporary=$(mktemp "$backup_directory/.gaudere-state.XXXXXX")
cleanup()
{
    rm -f "$temporary"
}
trap cleanup EXIT HUP INT TERM

tar --exclude='./state.db.lock' -czf "$temporary" -C "$state_directory" .
mv "$temporary" "$archive"
trap - EXIT HUP INT TERM

archive_name=$(basename "$archive")
(
    cd "$backup_directory"
    sha256sum "$archive_name" > "$archive_name.sha256"
)

printf '%s\n' "$archive"
