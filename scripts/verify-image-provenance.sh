#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}

fail()
{
    printf 'gaudere image provenance: %s\n' "$*" >&2
    exit 1
}

valid_commit()
{
    value=$1
    case "$value" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#value}" -eq 40 ]
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

[ "$#" -ge 3 ] && [ "$#" -le 4 ] \
    || fail "usage: $0 IMAGE EXPECTED_AGENT_REF EXPECTED_CORE_REF [EXPECTED_IMAGE_ID]"

image=$1
expected_agent_ref=$2
expected_core_ref=$3
expected_image_id=${4:-}

[ -n "$image" ] || fail "image reference must not be empty"
case "$image" in
    *[!A-Za-z0-9._:/@-]*) fail "image reference contains unsupported characters" ;;
esac
valid_commit "$expected_agent_ref" \
    || fail "expected Agent revision must be one 40-character lowercase Git SHA"
valid_commit "$expected_core_ref" \
    || fail "expected Core revision must be one 40-character lowercase Git SHA"
if [ -n "$expected_image_id" ]; then
    valid_image_id "$expected_image_id" \
        || fail "expected image ID must be one full sha256 image ID"
fi

command -v "$podman_command" >/dev/null 2>&1 \
    || fail "required command not found: $podman_command"
"$podman_command" image exists "$image" \
    || fail "image does not exist: $image"

image_id=$("$podman_command" image inspect --format '{{.Id}}' "$image") \
    || fail "cannot resolve image ID: $image"
valid_image_id "$image_id" \
    || fail "image did not resolve to one full sha256 ID: $image"
if [ -n "$expected_image_id" ] && [ "$image_id" != "$expected_image_id" ]; then
    fail "image ID mismatch for $image (expected $expected_image_id, found $image_id)"
fi

oci_agent_ref=$("$podman_command" image inspect \
    --format '{{ index .Labels "org.opencontainers.image.revision" }}' "$image") \
    || fail "cannot inspect OCI Agent revision label: $image"
agent_ref=$("$podman_command" image inspect \
    --format '{{ index .Labels "io.gaudere.agent.revision" }}' "$image") \
    || fail "cannot inspect Gaudere Agent revision label: $image"
core_ref=$("$podman_command" image inspect \
    --format '{{ index .Labels "io.gaudere.core.revision" }}' "$image") \
    || fail "cannot inspect Gaudere Core revision label: $image"

[ "$oci_agent_ref" = "$expected_agent_ref" ] \
    || fail "OCI Agent revision mismatch for $image"
[ "$agent_ref" = "$expected_agent_ref" ] \
    || fail "Gaudere Agent revision mismatch for $image"
[ "$core_ref" = "$expected_core_ref" ] \
    || fail "Gaudere Core revision mismatch for $image"

printf 'image=%s\n' "$image"
printf 'image_id=%s\n' "$image_id"
printf 'agent_ref=%s\n' "$agent_ref"
printf 'core_ref=%s\n' "$core_ref"
printf 'image_provenance=PASS\n'
