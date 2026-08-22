#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
container=${GAUDERE_CONTAINER:-gaudere-agent}
socket=${GAUDERE_CONTROL_SOCKET:-/tmp/gaudere-control.sock}

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 echo ID TEXT | openai ID TEXT | reflect ID OBJECTIVE | task ID | budget | accept-wake SOURCE_TASK_ID | revoke-wake WAKE_ID REASON | wake WAKE_ID" >&2
    exit 2
fi

exec "$podman_command" exec "$container" \
    /usr/local/bin/gaudere-control --socket "$socket" "$@"
