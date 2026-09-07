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
# Bootstrap execution envelope for the pinned 4.6 GiB GGUF. Fedora production
# proved that the pre-Goose 256 MiB cgroup budget OOM-kills Goose while mapping
# the model. Keep a finite bound with headroom for llama.cpp/KV/runtime state.
goose_memory=12G
goose_memory_swap=14G

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

python3 - "$target_quadlet" "$rendered_quadlet" "$goose_root" "$container_root" "$host_model_cache" "$container_model_cache" "$goose_memory" "$goose_memory_swap" <<'PY'
from pathlib import Path
import re
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
host_root, container_root, host_cache, container_cache, target_memory, target_swap = sys.argv[3:]
lines = source.read_text(encoding='utf-8').splitlines()
original = list(lines)

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

if lines[root_indexes[0]] == old_root:
    if cache_ro in lines:
        raise SystemExit('read-only model-cache mount exists while Goose root is not writable')
    lines[root_indexes[0]] = new_root
    lines.insert(root_indexes[0] + 1, cache_ro)
elif cache_ro not in lines:
    lines.insert(root_indexes[0] + 1, cache_ro)

size_re = re.compile(r'^(\d+)([KMGT])$', re.IGNORECASE)

def size_bytes(value: str) -> int:
    match = size_re.fullmatch(value.strip())
    if not match:
        raise SystemExit(f'unsupported finite Quadlet memory size: {value}')
    number = int(match.group(1))
    scale = {'K': 1024, 'M': 1024**2, 'G': 1024**3, 'T': 1024**4}[match.group(2).upper()]
    return number * scale

memory_indexes = [i for i, line in enumerate(lines) if line.startswith('Memory=')]
swap_indexes = [i for i, line in enumerate(lines) if line.startswith('MemorySwap=')]
if len(memory_indexes) != 1 or len(swap_indexes) != 1:
    raise SystemExit('target Quadlet must contain exactly one Memory= and one MemorySwap= line')

memory_index = memory_indexes[0]
swap_index = swap_indexes[0]
current_memory = lines[memory_index].split('=', 1)[1]
current_swap = lines[swap_index].split('=', 1)[1]
if size_bytes(current_memory) < size_bytes(target_memory):
    lines[memory_index] = f'Memory={target_memory}'
if size_bytes(current_swap) < size_bytes(target_swap):
    lines[swap_index] = f'MemorySwap={target_swap}'
# Podman requires memory-swap >= memory. Prove that after normalization.
final_memory = lines[memory_index].split('=', 1)[1]
final_swap = lines[swap_index].split('=', 1)[1]
if size_bytes(final_swap) < size_bytes(final_memory):
    raise SystemExit('final MemorySwap must be greater than or equal to Memory')

rendered = '\n'.join(lines) + '\n'

# Mechanically prove that only the Goose root mount layout and finite resource
# budget may change. Everything else, especially provider/network authority,
# must remain byte-for-byte equivalent after normalization.
def normalize(values):
    out = []
    for line in values:
        if line in (old_root, new_root):
            out.append('Volume=<goose-root-layout>')
        elif line == cache_ro:
            continue
        elif line.startswith('Memory='):
            out.append('Memory=<goose-bootstrap-budget>')
        elif line.startswith('MemorySwap='):
            out.append('MemorySwap=<goose-bootstrap-budget>')
        else:
            out.append(line)
    return out
if normalize(original) != normalize(rendered.splitlines()):
    raise SystemExit('runtime/model/resource repair attempted an unauthorized Quadlet mutation')

destination.write_text(rendered, encoding='utf-8')
print('layout_and_budget=repaired' if original != lines else 'layout_and_budget=already-correct')
print(f'memory={final_memory}')
print(f'memory_swap={final_swap}')
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
printf 'gaudere local-goose runtime mount repair: memory_floor=%s\n' "$goose_memory"
printf 'gaudere local-goose runtime mount repair: memory_swap_floor=%s\n' "$goose_memory_swap"
printf 'gaudere local-goose runtime mount repair: provider_authority=OFF\n'
printf 'gaudere local-goose runtime mount repair: network=none\n'
printf 'gaudere local-goose runtime mount repair: service remains stopped\n'
