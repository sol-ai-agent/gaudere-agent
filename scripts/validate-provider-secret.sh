#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/provider-secret.XXXXXX")
state_directory="$workspace/state"
mkdir -p "$state_directory"
secret_name="gaudere-openai-validation-$(date +%s)-$$"
secret_target="validation-openai-key"
secret_created=0

cleanup()
{
    if [ "$secret_created" = "1" ]; then
        "$podman_command" secret rm "$secret_name" >/dev/null 2>&1 || true
    fi
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
    if ! printf '%s\n' "$text" | grep -q "$pattern"; then
        printf 'validation failure: expected pattern %s\n' "$pattern" >&2
        printf '%s\n' "$text" >&2
        exit 1
    fi
}

run_check_with_mode()
{
    mode=$1
    "$podman_command" run --rm \
        --network none \
        --userns keep-id \
        --read-only \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        -v "$state_directory:/var/lib/gaudere:Z" \
        --secret "$secret_name,target=$secret_target,uid=1000,gid=1000,mode=$mode" \
        "$image" \
        --state /var/lib/gaudere/state.db \
        --check \
        --openai-model gpt-test \
        --openai-secret "$secret_target"
}

if ! "$podman_command" image exists "$image"; then
    printf 'validation failure: image %s does not exist\n' "$image" >&2
    exit 1
fi

say "create synthetic Podman secret from stdin"
printf '%s' 'synthetic-openai-validation-key' \
    | "$podman_command" secret create "$secret_name" - >/dev/null
secret_created=1

say "prove broad 0444 secret mode is rejected"
if broad_output=$(run_check_with_mode 444 2>&1); then
    printf 'validation failure: provider activation accepted mode 0444 secret\n' >&2
    printf '%s\n' "$broad_output" >&2
    exit 1
fi
expect_line "$broad_output" 'grants permissions to group or other users'

say "prove private 0400 secret mode passes provider preflight"
private_output=$(run_check_with_mode 400 2>&1)
expect_line "$private_output" \
    'gaudere-agent: OpenAI provider enabled model=gpt-test secret=validation-openai-key'
expect_line "$private_output" 'gaudere-agent: running'
expect_line "$private_output" 'gaudere-agent: safe'

say "validation complete"
printf 'gaudere provider secret validation: PASS\n'
