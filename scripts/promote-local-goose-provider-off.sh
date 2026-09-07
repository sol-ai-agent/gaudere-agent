#!/bin/sh
set -eu

systemctl_command=${SYSTEMCTL:-systemctl}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
base_promotion="$script_directory/promote-local-goose-provider-off-base.sh"
runtime_repair="$script_directory/repair-local-goose-runtime-mount.sh"

fail()
{
    printf 'gaudere local-goose provider-off promotion: %s\n' "$*" >&2
    exit 1
}

[ -f "$target_quadlet" ] && [ ! -L "$target_quadlet" ] \
    || fail "target Quadlet must be an existing regular non-symlink file: $target_quadlet"
[ -f "$base_promotion" ] || fail "base promotion helper is missing: $base_promotion"
[ -x "$runtime_repair" ] || fail "runtime/model mount repair is missing or not executable: $runtime_repair"

previous_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-local-goose-pre-promotion.XXXXXX")
install -m 0600 "$target_quadlet" "$previous_quadlet"
cleanup()
{
    rm -f -- "$previous_quadlet"
}
trap cleanup EXIT
trap 'cleanup; exit 1' HUP INT TERM

if ! sh "$base_promotion"; then
    fail "base local-Goose promotion failed"
fi

if ! "$runtime_repair"; then
    install -m 0600 "$previous_quadlet" "$target_quadlet" || true
    "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
    fail "runtime/model mount split failed; previous Quadlet restored"
fi

cleanup
trap - EXIT HUP INT TERM
printf 'gaudere local-goose provider-off promotion: runtime_mount=writable\n'
printf 'gaudere local-goose provider-off promotion: model_cache_mount=read-only\n'
