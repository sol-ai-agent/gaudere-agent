#!/bin/sh
set -eu

engine=${CONTAINER_ENGINE:-podman}
image=${GAUDERE_FEDORA44_BUILD_IMAGE:-localhost/gaudere-fedora44-build:44}
repo=$(git rev-parse --show-toplevel)
prefix_rel=${GAUDERE_LOCAL_PREFIX_REL:-.gaudere-local/fedora44}
build_rel=${GAUDERE_BUILD_ROOT_REL:-.build-fedora44}

for relative in "$prefix_rel" "$build_rel"; do
    case "$relative" in
        /*|..|../*|*/../*|*/..)
            echo "local prefix/build root must stay inside the repository" >&2
            exit 1
            ;;
    esac
done

"$engine" build \
    --file "$repo/Containerfile.fedora44-build" \
    --tag "$image" \
    "$repo"

volume="$repo:/workspace/agent"
if [ "$(basename "$engine")" = podman ]; then
    volume="$volume:Z"
fi

"$engine" run --rm \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --env GAUDERE_LOCAL_PREFIX="/workspace/agent/$prefix_rel" \
    --env GAUDERE_BUILD_ROOT="/workspace/agent/$build_rel" \
    --volume "$volume" \
    --workdir /workspace/agent \
    "$image" \
    /bin/sh /workspace/agent/scripts/fedora44-build-inside.sh
