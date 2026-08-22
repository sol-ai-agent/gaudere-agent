#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
current_image=${GAUDERE_CURRENT_IMAGE:-localhost/gaudere-agent:dev}
rollback_image=${GAUDERE_ROLLBACK_IMAGE:-}
manifest=${GAUDERE_ROLLBACK_MANIFEST:-}
temporary_manifest=

fail()
{
    printf 'gaudere image rollback capture: %s\n' "$*" >&2
    exit 1
}

valid_image_ref()
{
    value=$1
    [ -n "$value" ] || return 1
    case "$value" in
        *[!A-Za-z0-9._:/@-]*) return 1 ;;
    esac
}

valid_image_id()
{
    value=$1
    case "$value" in
        sha256:*) digest=${value#sha256:} ;;
        *) return 1 ;;
    esac
    case "$digest" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#digest}" -eq 64 ]
}

cleanup()
{
    [ -z "$temporary_manifest" ] || rm -f "$temporary_manifest"
}
trap cleanup EXIT HUP INT TERM

[ "$#" -eq 0 ] || fail "usage: GAUDERE_ROLLBACK_IMAGE=... GAUDERE_ROLLBACK_MANIFEST=... $0"
valid_image_ref "$current_image" || fail "current image reference is invalid"
valid_image_ref "$rollback_image" \
    || fail "GAUDERE_ROLLBACK_IMAGE must name an explicit rollback tag"
[ "$rollback_image" != "$current_image" ] \
    || fail "rollback tag must differ from the current mutable image reference"
[ -n "$manifest" ] \
    || fail "GAUDERE_ROLLBACK_MANIFEST must name a durable manifest path"
[ ! -e "$manifest" ] \
    || fail "rollback manifest already exists: $manifest"

command -v "$podman_command" >/dev/null 2>&1 \
    || fail "required command not found: $podman_command"
for command in date dirname ln mkdir mktemp rm; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done

"$podman_command" image exists "$current_image" \
    || fail "current image does not exist: $current_image"
if "$podman_command" image exists "$rollback_image" >/dev/null 2>&1; then
    fail "rollback tag already exists and will not be overwritten: $rollback_image"
fi

current_image_id=$("$podman_command" image inspect --format '{{.Id}}' "$current_image") \
    || fail "cannot resolve current image ID"
valid_image_id "$current_image_id" \
    || fail "current image did not resolve to one full sha256 ID"

# Tag the captured immutable ID, never the mutable name. This operation must happen
# before any candidate build that could move localhost/gaudere-agent:dev.
"$podman_command" tag "$current_image_id" "$rollback_image" \
    || fail "cannot create rollback tag"
rollback_image_id=$("$podman_command" image inspect --format '{{.Id}}' "$rollback_image") \
    || fail "cannot resolve rollback image ID"
[ "$rollback_image_id" = "$current_image_id" ] \
    || fail "rollback tag does not resolve to the captured image ID"

manifest_directory=$(dirname "$manifest")
mkdir -p "$manifest_directory"
temporary_manifest=$(mktemp "$manifest_directory/.gaudere-image-rollback.XXXXXX")
chmod 600 "$temporary_manifest"
captured_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
    printf 'schema=gaudere.image-rollback.v1\n'
    printf 'captured_at_utc=%s\n' "$captured_at"
    printf 'source_image=%s\n' "$current_image"
    printf 'source_image_id=%s\n' "$current_image_id"
    printf 'rollback_image=%s\n' "$rollback_image"
    printf 'rollback_image_id=%s\n' "$rollback_image_id"
} > "$temporary_manifest"

# An atomic hard-link publication refuses an existing destination and never
# exposes a partially written manifest.
ln "$temporary_manifest" "$manifest" 2>/dev/null \
    || fail "cannot publish rollback manifest without overwriting: $manifest"
rm -f "$temporary_manifest"
temporary_manifest=

printf 'rollback_image=%s\n' "$rollback_image"
printf 'rollback_image_id=%s\n' "$rollback_image_id"
printf 'rollback_manifest=%s\n' "$manifest"
printf 'rollback_capture=PASS\n'
