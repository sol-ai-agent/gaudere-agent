#!/bin/sh
set -eu

systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
goose_root=${GAUDERE_GOOSE_ROOT:-$HOME/.local/share/gaudere/goose}
container_root=/var/lib/gaudere/goose
host_model_cache=$goose_root/cache/huggingface
container_model_cache=$container_root/cache/huggingface

fail()
{
    printf 'gaudere local-goose runtime mount repair: %s\n' "$*" >&2
    exit 1
}

require_service_stopped()
{
    service_state=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
    case "$service_state" in
        active|activating|reloading)
            fail "$service_name must be stopped before local Goose runtime mount repair"
            ;;
    esac
}

case "$goose_root" in
    /*) ;;
    *) fail "GAUDERE_GOOSE_ROOT must be absolute" ;;
esac
for command in "$systemctl_command" python3 install mktemp rm; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
[ -d "$goose_root" ] && [ ! -L "$goose_root" ] \
    || fail "Goose root must be an existing non-symlink directory: $goose_root"
[ -d "$host_model_cache" ] && [ ! -L "$host_model_cache" ] \
    || fail "Goose model cache must be an existing non-symlink directory: $host_model_cache"
[ -f "$target_quadlet" ] && [ ! -L "$target_quadlet" ] \
    || fail "target Quadlet must be an existing regular non-symlink file: $target_quadlet"

require_service_stopped

rendered_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-local-goose-runtime.XXXXXX")
previous_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-local-goose-runtime-previous.XXXXXX")
install -m 0600 "$target_quadlet" "$previous_quadlet"
cleanup()
{
    rm -f -- "$rendered_quadlet" "$previous_quadlet"
}
trap cleanup EXIT
trap 'cleanup; exit 1' HUP INT TERM

python3 - "$target_quadlet" "$rendered_quadlet" "$goose_root" "$container_root" "$host_model_cache" "$container_model_cache" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
host_root, container_root, host_cache, container_cache = sys.argv[3:]
lines = source.read_text(encoding='utf-8').splitlines()

images = [line for line in lines if line.startswith('Image=')]
execs = [line for line in lines if line.startswith('Exec=')]
if len(images) != 1 or len(execs) != 1:
    raise SystemExit('target Quadlet must contain exactly one Image= and one Exec= line')
exec_line = execs[0]
for required in ('--local-activity-sidecar /var/lib/gaudere/local-activity-pulse.db',
                 '--local-goose-model ', '--local-goose-model-sha256 ',
                 '--local-goose-governance /var/lib/gaudere/goose-governance.db'):
    if required not in exec_line:
        raise SystemExit(f'target Quadlet is not the expected local Goose profile: {required.strip()}')
for forbidden in ('--openai-model ', '--autonomous-pulse-provider', '--wake-intents'):
    if forbidden in exec_line:
        raise SystemExit(f'target Quadlet contains forbidden provider/external authority: {forbidden.strip()}')
for required in ('Network=none', 'ReadOnly=true', 'NoNewPrivileges=true', 'DropCapability=all'):
    if required not in lines:
        raise SystemExit(f'target Quadlet lost required hardening: {required}')

old_root = f'Volume={host_root}:{container_root}:ro,Z'
new_root = f'Volume={host_root}:{container_root}:rw,Z'
cache_ro = f'Volume={host_cache}:{container_cache}:ro'
root_indexes = [i for i, line in enumerate(lines) if line in (old_root, new_root)]
if len(root_indexes) != 1:
    raise SystemExit('target Quadlet must contain exactly one recognized Goose root mount')
for line in lines:
    if line.startswith('Volume=') and f':{container_root}' in line:
        if line not in (old_root, new_root, cache_ro):
            raise SystemExit(f'unexpected mount overlaps Goose runtime/model tree: {line}')

if lines[root_indexes[0]] == new_root and cache_ro in lines:
    destination.write_text(source.read_text(encoding='utf-8'), encoding='utf-8')
    print('mount_layout=already-correct')
    raise SystemExit(0)
if cache_ro in lines:
    raise SystemExit('read-only model-cache mount exists while Goose root is not writable')

lines[root_indexes[0]] = new_root
lines.insert(root_indexes[0] + 1, cache_ro)
rendered = '\n'.join(lines) + '\n'

# Mechanically prove that only the Goose root mode changed and one nested
# read-only model-cache mount was inserted.
def normalize(values, repaired):
    out = []
    for line in values:
        if line in (old_root, new_root):
            out.append('Volume=<goose-root-layout>')
        elif repaired and line == cache_ro:
            continue
        else:
            out.append(line)
    return out
if normalize(source.read_text(encoding='utf-8').splitlines(), False) != normalize(rendered.splitlines(), True):
    raise SystemExit('runtime/model mount repair attempted an unauthorized Quadlet mutation')

destination.write_text(rendered, encoding='utf-8')
print('mount_layout=repaired')
PY

require_service_stopped
install -m 0600 "$rendered_quadlet" "$target_quadlet"
if ! "$systemctl_command" --user daemon-reload; then
    install -m 0600 "$previous_quadlet" "$target_quadlet" || true
    "$systemctl_command" --user daemon-reload >/dev/null 2>&1 || true
    fail "daemon-reload failed; previous Quadlet restored"
fi

cleanup
trap - EXIT HUP INT TERM
printf 'gaudere local-goose runtime mount repair: runtime_mount=writable\n'
printf 'gaudere local-goose runtime mount repair: model_cache_mount=read-only\n'
printf 'gaudere local-goose runtime mount repair: provider_authority=OFF\n'
printf 'gaudere local-goose runtime mount repair: network=none\n'
printf 'gaudere local-goose runtime mount repair: service remains stopped\n'
