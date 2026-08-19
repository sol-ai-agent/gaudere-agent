#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/live-control.XXXXXX")
state_directory="$workspace/state"
mkdir -p "$state_directory"
container="gaudere-live-control-validation-$$"
socket=/tmp/gaudere-control.sock

cleanup()
{
    "$podman_command" rm -f "$container" >/dev/null 2>&1 || true
    if [ "${KEEP_GAUDERE_VALIDATION_STATE:-0}" = "1" ]; then
        printf 'validation state kept at %s\n' "$workspace" >&2
    else
        rm -rf "$workspace"
    fi
}
trap cleanup EXIT HUP INT TERM

say()
{
    printf '\n==> %s\n' "$*"
}

expect_file_line()
{
    file=$1
    pattern=$2
    if ! grep -Eq "$pattern" "$file"; then
        printf 'validation failure: expected pattern %s in %s\n' "$pattern" "$file" >&2
        cat "$file" >&2
        exit 1
    fi
}

if ! command -v "$podman_command" >/dev/null 2>&1; then
    printf 'validation failure: %s is required\n' "$podman_command" >&2
    exit 1
fi
if ! "$podman_command" image exists "$image"; then
    printf 'validation failure: image %s does not exist\n' "$image" >&2
    exit 1
fi

help_output=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    "$image" --help 2>&1 || true)
if ! printf '%s\n' "$help_output" | grep -q -- '--control-socket'; then
    printf 'validation failure: image %s predates live-control support; rebuild current main first\n' "$image" >&2
    exit 1
fi

say "start disposable offline owner service"
"$podman_command" run -d --name "$container" \
    --network none \
    --userns keep-id \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    --pids-limit 64 \
    --memory 256m \
    -v "$state_directory:/var/lib/gaudere:Z" \
    "$image" \
    --state /var/lib/gaudere/state.db \
    --control-socket "$socket" >/dev/null

i=0
while ! "$podman_command" exec "$container" sh -c "test -S '$socket'" >/dev/null 2>&1; do
    if ! "$podman_command" container exists "$container"; then
        printf 'validation failure: disposable service disappeared before socket readiness\n' >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 100 ]; then
        printf 'validation failure: live control socket did not become ready\n' >&2
        "$podman_command" logs "$container" >&2 || true
        exit 1
    fi
    sleep 0.05
done

say "submit local.echo through live control"
"$podman_command" exec "$container" \
    /usr/local/bin/gaudere-control --socket "$socket" \
    echo validation-live-control "hello from live control" \
    >"$workspace/submit" 2>&1
expect_file_line "$workspace/submit" '^status=pending$'

say "prove provider command is gated while service is offline"
if "$podman_command" exec "$container" \
    /usr/local/bin/gaudere-control --socket "$socket" \
    openai validation-live-ai "must remain offline" \
    >"$workspace/openai-disabled" 2>&1; then
    printf 'validation failure: provider command unexpectedly succeeded\n' >&2
    exit 1
fi
expect_file_line "$workspace/openai-disabled" 'OpenAI provider is not enabled'

say "inspect durable result through owner process"
i=0
while :; do
    "$podman_command" exec "$container" \
        /usr/local/bin/gaudere-control --socket "$socket" \
        task validation-live-control >"$workspace/report" 2>&1
    if grep -q '^status=succeeded$' "$workspace/report"; then
        break
    fi
    i=$((i + 1))
    if [ "$i" -ge 100 ]; then
        printf 'validation failure: live echo did not reach succeeded\n' >&2
        cat "$workspace/report" >&2
        exit 1
    fi
    sleep 0.05
done
expect_file_line "$workspace/report" '^attempts=1/1$'
expect_file_line "$workspace/report" '^result_output="hello from live control"$'

say "prove direct second Runtime owner is still rejected"
if "$podman_command" exec "$container" \
    /usr/local/bin/gaudere-agent --state /var/lib/gaudere/state.db \
    --task validation-live-control >"$workspace/direct-owner" 2>&1; then
    printf 'validation failure: second process unexpectedly opened live state DB\n' >&2
    exit 1
fi
expect_file_line "$workspace/direct-owner" 'state database is already owned'

say "stop disposable service cleanly"
"$podman_command" stop --time 10 "$container" >/dev/null
"$podman_command" logs "$container" >"$workspace/logs" 2>&1
expect_file_line "$workspace/logs" 'gaudere-agent: control socket=/tmp/gaudere-control.sock'
expect_file_line "$workspace/logs" 'gaudere-agent: shutdown requested by signal 15'
expect_file_line "$workspace/logs" 'gaudere-agent: safe'
"$podman_command" rm "$container" >/dev/null

say "validation complete"
printf 'gaudere live control validation: PASS\n'
