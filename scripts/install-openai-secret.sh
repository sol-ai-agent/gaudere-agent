#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
secret_name=${GAUDERE_OPENAI_SECRET_NAME:-gaudere-openai-api-key}
replace=0

usage()
{
    printf 'Usage: %s [--replace]\n' "$0" >&2
}

if [ "${1:-}" = "--replace" ]; then
    replace=1
    shift
fi
if [ "$#" -ne 0 ]; then
    usage
    exit 2
fi

if ! command -v "$podman_command" >/dev/null 2>&1; then
    printf 'gaudere secret install: %s is required\n' "$podman_command" >&2
    exit 1
fi
if [ ! -r /dev/tty ] || [ ! -w /dev/tty ]; then
    printf 'gaudere secret install: an interactive terminal is required\n' >&2
    exit 1
fi

if "$podman_command" secret exists "$secret_name"; then
    if [ "$replace" != "1" ]; then
        printf 'gaudere secret install: Podman secret %s already exists; use --replace only for deliberate rotation\n' "$secret_name" >&2
        exit 1
    fi
fi

echo_disabled=0
restore_terminal()
{
    if [ "$echo_disabled" = "1" ]; then
        stty echo < /dev/tty || true
        printf '\n' > /dev/tty
        echo_disabled=0
    fi
}
trap restore_terminal EXIT HUP INT TERM

printf 'Paste OpenAI API key (input hidden): ' > /dev/tty
stty -echo < /dev/tty
echo_disabled=1
IFS= read -r secret_value < /dev/tty || {
    restore_terminal
    printf 'gaudere secret install: could not read key\n' >&2
    exit 1
}
restore_terminal

if [ -z "$secret_value" ]; then
    printf 'gaudere secret install: key must not be empty\n' >&2
    exit 1
fi
if ! LC_ALL=C printf '%s' "$secret_value" | grep -Eq '^[!-~]+$'; then
    printf 'gaudere secret install: key must be printable single-line ASCII without whitespace\n' >&2
    exit 1
fi

if [ "$replace" = "1" ]; then
    LC_ALL=C printf '%s' "$secret_value" \
        | "$podman_command" secret create --replace \
            --label gaudere-purpose=openai-api \
            "$secret_name" - >/dev/null
else
    LC_ALL=C printf '%s' "$secret_value" \
        | "$podman_command" secret create \
            --label gaudere-purpose=openai-api \
            "$secret_name" - >/dev/null
fi

# POSIX shells cannot guarantee memory zeroization of variables, but unset the
# transient copy immediately after Podman has consumed stdin.
unset secret_value

printf 'gaudere secret install: installed Podman secret %s\n' "$secret_name"
printf 'gaudere secret install: the secret value was not written to Git, argv, or normal environment configuration\n'
