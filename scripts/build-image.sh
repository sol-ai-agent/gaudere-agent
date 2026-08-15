#!/bin/sh
set -eu

if ! command -v podman >/dev/null 2>&1; then
    echo "podman is required" >&2
    exit 1
fi

podman build \
    --tag localhost/gaudere-agent:dev \
    --file Containerfile \
    .
