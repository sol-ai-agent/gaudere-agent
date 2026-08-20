#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
container=${GAUDERE_CONTAINER:-gaudere-agent}
socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 echo ID TEXT | openai ID TEXT | task ID | budget" >&2
    exit 2
fi

exec "$podman_command" exec "$container" \
    /usr/local/bin/gaudere-control --socket "$socket" "$@"
