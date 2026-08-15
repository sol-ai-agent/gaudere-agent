#!/bin/sh
set -eu

if ! command -v podman >/dev/null 2>&1; then
    echo "podman is required" >&2
    exit 1
fi

builder_image="registry.fedoraproject.org/fedora:44"
if podman image exists localhost/al_openai_cpp:10.0.0; then
    builder_image="localhost/al_openai_cpp:10.0.0"
    echo "Reusing local builder image: $builder_image"
else
    echo "Local C++ builder not found; using: $builder_image"
fi

podman build \
    --build-arg "BUILDER_IMAGE=$builder_image" \
    --tag localhost/gaudere-agent:dev \
    --file Containerfile \
    .
