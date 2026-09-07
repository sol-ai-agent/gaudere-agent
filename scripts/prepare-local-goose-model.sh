#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
image_tag=${GAUDERE_IMAGE_TAG:-localhost/gaudere-agent:dev}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
goose_root=${GAUDERE_GOOSE_ROOT:-$HOME/.local/share/gaudere/goose}
container_root=/var/lib/gaudere/goose
huggingface_home=$container_root/cache/huggingface

if ! command -v "$podman_command" >/dev/null 2>&1; then
    echo "podman is required" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required" >&2
    exit 1
fi

case "$goose_root" in
    /*) ;;
    *) echo "GAUDERE_GOOSE_ROOT must be absolute" >&2; exit 1 ;;
esac
case "$model_id" in
    ''|*[!A-Za-z0-9._:/-]*)
        echo "GAUDERE_GOOSE_MODEL_ID contains unsupported characters" >&2
        exit 1
        ;;
esac

mkdir -p "$goose_root"
chmod 700 "$goose_root"

# A previous interrupted/incorrect preparation may have persisted a registry
# entry whose GGUF lived only in an ephemeral container cache. Remove only the
# exact stale model entry before asking pinned Goose to download it again.
python3 - "$goose_root" "$container_root" "$model_id" <<'PY'
import json
import os
from pathlib import Path
import sys

host_root = Path(sys.argv[1]).resolve()
container_root = sys.argv[2].rstrip('/')
model_id = sys.argv[3]
registry = host_root / 'data' / 'models' / 'registry.json'
if not registry.is_file():
    raise SystemExit(0)

doc = json.loads(registry.read_text(encoding='utf-8'))
models = doc.get('models')
if not isinstance(models, list):
    raise SystemExit(f'Goose registry models field is invalid: {registry}')

kept = []
removed = False
for entry in models:
    if not isinstance(entry, dict) or entry.get('id') != model_id:
        kept.append(entry)
        continue

    registered = entry.get('local_path')
    if isinstance(registered, str) and registered.startswith(container_root + '/'):
        relative = registered[len(container_root) + 1:]
        if (host_root / relative).is_file():
            kept.append(entry)
            continue

    removed = True

if removed:
    doc['models'] = kept
    temporary = registry.with_name(registry.name + '.tmp')
    temporary.write_text(json.dumps(doc, separators=(',', ':')) + '\n', encoding='utf-8')
    os.chmod(temporary, 0o600)
    temporary.replace(registry)
    print(f'Removed stale Goose registry entry: {model_id}')
PY

echo "Preparing Goose model: $model_id"
echo "Persistent Goose root: $goose_root"

"$podman_command" run --rm \
    --userns=keep-id \
    --security-opt=no-new-privileges \
    --cap-drop=all \
    --entrypoint /usr/local/bin/goose \
    --env "GOOSE_PATH_ROOT=$container_root" \
    --env "HF_HOME=$huggingface_home" \
    --env "XDG_CACHE_HOME=$container_root/cache" \
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
