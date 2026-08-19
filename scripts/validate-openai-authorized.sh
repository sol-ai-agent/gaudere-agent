#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
model=${GAUDERE_OPENAI_VALIDATION_MODEL:-gpt-5.6-sol}
secret_name=${GAUDERE_OPENAI_SECRET_NAME:-gaudere-openai-api-key}
secret_target="gaudere-openai-api-key"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/openai-authorized.XXXXXX")
state_directory="$workspace/state"
mkdir -p "$state_directory"

cleanup()
{
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

expect_line()
{
    text=$1
    pattern=$2
    if ! printf '%s\n' "$text" | grep -Eq "$pattern"; then
        printf 'validation failure: expected pattern %s\n' "$pattern" >&2
        printf '%s\n' "$text" >&2
        exit 1
    fi
}

if ! "$podman_command" image exists "$image"; then
    printf 'validation failure: image %s does not exist\n' "$image" >&2
    exit 1
fi
if ! "$podman_command" secret exists "$secret_name"; then
    printf 'validation failure: Podman secret %s does not exist\n' "$secret_name" >&2
    printf 'install it with scripts/install-openai-secret.sh after creating the restricted OpenAI project key\n' >&2
    exit 1
fi

help_output=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    "$image" --help 2>&1 || true)
if ! printf '%s\n' "$help_output" | grep -q -- '--openai-once'; then
    printf 'validation failure: image %s predates OpenAI one-shot support; rebuild current main first\n' "$image" >&2
    exit 1
fi

say "perform one bounded authorized OpenAI call"
# This is intentionally a disposable container/state pair. It has normal outbound
# networking but publishes no inbound port. The key is mounted read-only from the
# named Podman secret; its value is never passed in argv or environment variables.
if ! output=$("$podman_command" run --rm \
        --userns keep-id \
        --read-only \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$state_directory:/var/lib/gaudere:Z" \
        --secret "$secret_name,target=$secret_target,uid=1000,gid=1000,mode=400" \
        "$image" \
        --state /var/lib/gaudere/state.db \
        --openai-once validation-openai-authorized \
            "Reply briefly in French. Include the exact word Gaudere." \
        --openai-model "$model" \
        --openai-secret "$secret_target" 2>&1); then
    printf 'validation failure: authorized one-shot process failed\n' >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

expect_line "$output" \
    "gaudere-agent: OpenAI provider enabled model=$model secret=$secret_target"
expect_line "$output" '^gaudere-agent: running$'
expect_line "$output" '^status=succeeded$'
expect_line "$output" '^attempts=1/2$'
expect_line "$output" '^result_content_type="text/plain; charset=utf-8"$'
expect_line "$output" '^result_output=".+"$'
expect_line "$output" '^gaudere-agent: safe$'

say "validation complete"
printf '%s\n' "$output"
printf 'gaudere authorized OpenAI validation: PASS\n'
