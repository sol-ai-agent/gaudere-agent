#!/bin/sh
set -eu

if ! command -v podman >/dev/null 2>&1; then
    echo "podman is required" >&2
    exit 1
fi

# Keep the normal host build aligned with CI and the Containerfile dependency
# names. Alternative builders remain possible, but only as an explicit operator
# choice; do not silently prefer an unrelated local image.
builder_image=${GAUDERE_BUILDER_IMAGE:-registry.fedoraproject.org/fedora:44}

if [ ! -f gaudere.ref ]; then
    echo "gaudere.ref is required" >&2
    exit 1
fi
gaudere_ref=$(tr -d '\r\n' < gaudere.ref)
case "$gaudere_ref" in
    *[!0-9a-f]*|'')
        echo "gaudere.ref must contain one lowercase hexadecimal Git commit SHA" >&2
        exit 1
        ;;
esac
if [ "${#gaudere_ref}" -ne 40 ]; then
    echo "gaudere.ref must contain one 40-character Git commit SHA" >&2
    exit 1
fi

echo "Using builder image: $builder_image"
echo "Using Gaudere ref: $gaudere_ref"

podman build \
    --build-arg "BUILDER_IMAGE=$builder_image" \
    --build-arg "GAUDERE_REF=$gaudere_ref" \
    --tag localhost/gaudere-agent:dev \
    --file Containerfile \
    .
