#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
validation_root="$data_home/gaudere/validation"
mkdir -p "$validation_root"
workspace=$(mktemp -d "$validation_root/provider-budget.XXXXXX")
state_directory="$workspace/state"
mkdir -p "$state_directory"
secret_name="gaudere-budget-validation-$(date +%s)-$$"
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

run_agent()
{
    "$podman_command" run --rm \
        --network none \
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
        "$@" \
        --openai-model gpt-test \
        --openai-secret "$secret_target"
}

if ! command -v "$podman_command" >/dev/null 2>&1; then
    printf 'validation failure: %s is required\n' "$podman_command" >&2
    exit 1
fi
if ! "$podman_command" image exists "$image"; then
    printf 'validation failure: image %s does not exist\n' "$image" >&2
    exit 1
fi

say "create synthetic Podman secret"
printf '%s' 'synthetic-openai-budget-validation-key' \
    | "$podman_command" secret create "$secret_name" - >/dev/null
secret_created=1

say "prove current image exposes bootstrap budget without consuming it"
run_agent --check >"$workspace/check" 2>&1
expect_file_line "$workspace/check" \
    '^gaudere-agent: OpenAI budget max_total=12 max_window=4 window_seconds=86400 min_interval_seconds=900$'
expect_file_line "$workspace/check" '^gaudere-agent: running$'
expect_file_line "$workspace/check" '^gaudere-agent: safe$'

say "consume one permit through the real provider path while offline"
run_agent --openai-once validation-budget-first "offline budget first" \
    >"$workspace/first" 2>&1
expect_file_line "$workspace/first" '^id="validation-budget-first"$'
expect_file_line "$workspace/first" '^kind="provider.openai.responses"$'
expect_file_line "$workspace/first" '^status=manual_review$'
expect_file_line "$workspace/first" '^attempts=1/2$'
expect_file_line "$workspace/first" '^gaudere-agent: safe$'

say "prove an immediate second new call is denied before transport"
run_agent --openai-once validation-budget-second "offline budget second" \
    >"$workspace/second" 2>&1
expect_file_line "$workspace/second" '^id="validation-budget-second"$'
expect_file_line "$workspace/second" '^kind="provider.openai.responses"$'
expect_file_line "$workspace/second" '^status=failed$'
expect_file_line "$workspace/second" '^attempts=1/2$'
expect_file_line "$workspace/second" '^failure_code="provider_budget_cooldown"$'
expect_file_line "$workspace/second" \
    '^failure_message="provider minimum call interval has not elapsed"$'
expect_file_line "$workspace/second" '^gaudere-agent: safe$'

if grep -Eq 'provider_effect_unknown|openai_transport|curl_|transport result' \
        "$workspace/second"; then
    printf 'validation failure: second task shows evidence of reaching transport\n' >&2
    cat "$workspace/second" >&2
    exit 1
fi

say "prove the denied task remains durably terminal on reuse"
run_agent --openai-once validation-budget-second "different text cannot bypass budget" \
    >"$workspace/reuse" 2>&1
expect_file_line "$workspace/reuse" '^status=failed$'
expect_file_line "$workspace/reuse" '^attempts=1/2$'
expect_file_line "$workspace/reuse" '^failure_code="provider_budget_cooldown"$'
expect_file_line "$workspace/reuse" '^gaudere-agent: safe$'

say "validation complete"
printf 'gaudere provider budget validation: PASS\n'
