#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
model=${GAUDERE_OPENAI_VALIDATION_MODEL:-gpt-5.6}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/openai-network.XXXXXX")
state_directory="$workspace/state"
mkdir -p "$state_directory"
secret_name="gaudere-openai-network-validation-$(date +%s)-$$"
secret_target="validation-openai-invalid-key"
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

say "create synthetic invalid OpenAI credential"
printf '%s' 'gaudere-synthetic-invalid-key-not-a-real-credential' \
    | "$podman_command" secret create "$secret_name" - >/dev/null
secret_created=1

say "perform one outbound OpenAI authentication probe"
# No ports are published. Unlike the offline validators, this disposable container
# intentionally uses Podman's normal outbound network so DNS/TLS/HTTP can reach the
# fixed OpenAI Responses endpoint. The credential is synthetic and deliberately
# invalid; success means receiving a definite 4xx API response, not model output.
if ! probe_output=$("$podman_command" run --rm \
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
        --openai-once validation-openai-network "Gaudere synthetic connectivity probe" \
        --openai-model "$model" \
        --openai-secret "$secret_target" 2>&1); then
    printf 'validation failure: one-shot process failed before a durable terminal report\n' >&2
    printf '%s\n' "$probe_output" >&2
    exit 1
fi

expect_line "$probe_output" \
    "gaudere-agent: OpenAI provider enabled model=$model secret=$secret_target"
expect_line "$probe_output" '^gaudere-agent: running$'
expect_line "$probe_output" '^status=failed$'
expect_line "$probe_output" '^attempts=1/2$'
expect_line "$probe_output" '^failure_code="openai_http_4[0-9][0-9]"$'
expect_line "$probe_output" '^gaudere-agent: safe$'

say "validation complete"
printf '%s\n' "$probe_output"
printf 'gaudere OpenAI network validation: PASS\n'
