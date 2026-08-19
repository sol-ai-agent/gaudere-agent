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
echo "Using builder image: $builder_image"

podman build \
    --build-arg "BUILDER_IMAGE=$builder_image" \
    --tag localhost/gaudere-agent:dev \
    --file Containerfile \
    .
