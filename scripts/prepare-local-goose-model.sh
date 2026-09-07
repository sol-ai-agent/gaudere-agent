#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image_tag=${GAUDERE_IMAGE_TAG:-localhost/gaudere-agent:dev}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
goose_root=${GAUDERE_GOOSE_ROOT:-$HOME/.local/share/gaudere/goose}
container_root=/var/lib/gaudere/goose

command -v "$podman_command" >/dev/null 2>&1 || {
    echo "podman is required" >&2
    return 1 2>/dev/null || false
}
command -v python3 >/dev/null 2>&1 || {
    echo "python3 is required" >&2
    return 1 2>/dev/null || false
}

case "$goose_root" in
    /*) ;;
    *) echo "GAUDERE_GOOSE_ROOT must be absolute" >&2; return 1 2>/dev/null || false ;;
esac
case "$model_id" in
    ''|*[!A-Za-z0-9._:/-]*)
        echo "GAUDERE_GOOSE_MODEL_ID contains unsupported characters" >&2
        return 1 2>/dev/null || false
        ;;
esac

mkdir -p "$goose_root"
chmod 700 "$goose_root"

echo "Preparing Goose model: $model_id"
echo "Persistent Goose root: $goose_root"

"$podman_command" run --rm \
    --userns=keep-id \
    --security-opt=no-new-privileges \
    --cap-drop=all \
    --entrypoint /usr/local/bin/goose \
    --env "GOOSE_PATH_ROOT=$container_root" \
    --volume "$goose_root:$container_root:Z" \
    "$image_tag" \
    local-models download "$model_id"

python3 - "$goose_root" "$container_root" "$model_id" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

host_root = Path(sys.argv[1]).resolve()
container_root = sys.argv[2].rstrip('/')
model_id = sys.argv[3]
registry = host_root / 'data' / 'models' / 'registry.json'
if not registry.is_file():
    raise SystemExit(f'Goose registry missing after download: {registry}')

doc = json.loads(registry.read_text(encoding='utf-8'))
entries = [entry for entry in doc.get('models', []) if entry.get('id') == model_id]
if len(entries) != 1:
    raise SystemExit(f'expected exactly one Goose registry entry for {model_id}, found {len(entries)}')
entry = entries[0]
shards = entry.get('shard_files') or []
if shards:
    raise SystemExit('first Gaudere Goose gate requires one primary GGUF; registry reports sharded model')

registered = entry.get('local_path')
if not isinstance(registered, str) or not registered.startswith(container_root + '/'):
    raise SystemExit(f'non-canonical Goose local_path for {model_id}: {registered!r}')
relative = registered[len(container_root) + 1:]
primary = host_root / relative
if not primary.is_file():
    raise SystemExit(f'registered primary GGUF missing on Fedora: {primary}')

h = hashlib.sha256()
with primary.open('rb') as stream:
    for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
        h.update(block)

digest = h.hexdigest()
print(f'GAUDERE_GOOSE_MODEL_ID={model_id}')
print(f'GAUDERE_GOOSE_MODEL_FILE={primary}')
print(f'GAUDERE_GOOSE_MODEL_SHA256={digest}')
print(f'GAUDERE_GOOSE_REGISTRY={registry}')
PY
