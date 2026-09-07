#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
target_quadlet=${GAUDERE_TARGET_QUADLET:-"$quadlet_directory/gaudere-agent.container"}
image=${GAUDERE_IMAGE:-}
expected_agent_ref=${GAUDERE_EXPECTED_AGENT_REF:-}
expected_core_ref=${GAUDERE_EXPECTED_CORE_REF:-}
model_id=${GAUDERE_GOOSE_MODEL_ID:-unsloth/gemma-4-E4B-it-GGUF:Q4_K_M}
model_sha256=${GAUDERE_GOOSE_MODEL_SHA256:-}
goose_root=${GAUDERE_GOOSE_ROOT:-$HOME/.local/share/gaudere/goose}
model_container_root=/var/lib/gaudere/goose
governance_container_path=/var/lib/gaudere/goose-governance.db
activity_container_path=/var/lib/gaudere/local-activity-pulse.db
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
provenance_verifier="$script_directory/verify-image-provenance.sh"

fail()
{
    printf 'gaudere local-goose provider-off promotion: %s\n' "$*" >&2
    exit 1
}

require_service_stopped()
{
    service_state=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
    case "$service_state" in
        active|activating|reloading)
            fail "$service_name must be stopped before local Goose promotion"
            ;;
    esac
}

[ -n "$image" ] || fail "GAUDERE_IMAGE is required"
[ -n "$expected_agent_ref" ] || fail "GAUDERE_EXPECTED_AGENT_REF is required"
[ -n "$expected_core_ref" ] || fail "GAUDERE_EXPECTED_CORE_REF is required"
[ -n "$model_sha256" ] || fail "GAUDERE_GOOSE_MODEL_SHA256 is required"
[ -x "$provenance_verifier" ] \
    || fail "image provenance verifier is required: $provenance_verifier"

case "$model_sha256" in
    *[!0-9a-f]*|'') fail "GAUDERE_GOOSE_MODEL_SHA256 must be lowercase hexadecimal" ;;
esac
[ "${#model_sha256}" -eq 64 ] || fail "GAUDERE_GOOSE_MODEL_SHA256 must contain 64 hex characters"
case "$model_id" in
    ''|*[!A-Za-z0-9._:/-]*) fail "GAUDERE_GOOSE_MODEL_ID contains unsupported characters" ;;
esac
case "$goose_root" in
    /*) ;;
    *) fail "GAUDERE_GOOSE_ROOT must be absolute" ;;
esac

for command in "$podman_command" "$systemctl_command" python3 install mktemp rm grep sed wc; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done

[ -d "$goose_root" ] && [ ! -L "$goose_root" ] \
    || fail "Goose root must be an existing non-symlink directory: $goose_root"
[ -f "$target_quadlet" ] && [ ! -L "$target_quadlet" ] \
    || fail "target Quadlet must be an existing regular non-symlink file: $target_quadlet"

# Verify the downloaded registry entry and the exact GGUF hash before granting
# the service read-only access to it.
python3 - "$goose_root" "$model_container_root" "$model_id" "$model_sha256" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

host_root = Path(sys.argv[1]).resolve()
container_root = sys.argv[2].rstrip('/')
model_id = sys.argv[3]
expected = sys.argv[4]
registry = host_root / 'data' / 'models' / 'registry.json'
if not registry.is_file():
    raise SystemExit(f'Goose registry missing: {registry}')
doc = json.loads(registry.read_text(encoding='utf-8'))
entries = [entry for entry in doc.get('models', []) if entry.get('id') == model_id]
if len(entries) != 1:
    raise SystemExit(f'expected exactly one registry entry for {model_id}, found {len(entries)}')
entry = entries[0]
if entry.get('shard_files'):
    raise SystemExit('first local Goose promotion refuses sharded primary models')
registered = entry.get('local_path')
if not isinstance(registered, str) or not registered.startswith(container_root + '/'):
    raise SystemExit(f'non-canonical registered local_path: {registered!r}')
primary = host_root / registered[len(container_root) + 1:]
if not primary.is_file():
    raise SystemExit(f'registered primary GGUF is missing: {primary}')
h = hashlib.sha256()
with primary.open('rb') as stream:
    for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
        h.update(block)
actual = h.hexdigest()
if actual != expected:
    raise SystemExit(f'GGUF SHA256 mismatch: expected {expected}, got {actual}')
print(f'verified_model_file={primary}')
print(f'verified_model_sha256={actual}')
PY

require_service_stopped

# Require the exact provider-free local-activity production profile before
# rendering any change. The promotion may add local Goose, but never OpenAI,
# WakeIntent or network authority.
python3 - "$target_quadlet" "$activity_container_path" "$model_container_root" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
activity_path = sys.argv[2]
model_root = sys.argv[3]
lines = path.read_text(encoding='utf-8').splitlines()
images = [line for line in lines if line.startswith('Image=')]
execs = [line for line in lines if line.startswith('Exec=')]
if len(images) != 1 or len(execs) != 1:
    raise SystemExit('target Quadlet must contain exactly one Image= and one Exec= line')
exec_line = execs[0]
if f'--local-activity-sidecar {activity_path}' not in exec_line:
    raise SystemExit('target Quadlet is not the expected local-activity profile')
for forbidden in ('--openai-model ', '--autonomous-pulse-provider', '--wake-intents', '--local-goose-model '):
    if forbidden in exec_line:
        raise SystemExit(f'target Quadlet contains forbidden/pre-existing authority: {forbidden.strip()}')
if 'Network=none' not in lines:
    raise SystemExit('target Quadlet must retain Network=none')
for required in ('ReadOnly=true', 'NoNewPrivileges=true', 'DropCapability=all'):
    if required not in lines:
        raise SystemExit(f'target Quadlet lost required hardening: {required}')
for line in lines:
    if line.startswith('Volume=') and f':{model_root}' in line:
        raise SystemExit('target Quadlet already mounts the local Goose model root')
PY

provenance_output=$(PODMAN="$podman_command" sh "$provenance_verifier" \
    "$image" "$expected_agent_ref" "$expected_core_ref") \
    || fail "candidate image provenance verification failed"
printf '%s\n' "$provenance_output"
image_id=$(printf '%s\n' "$provenance_output" | sed -n 's/^image_id=//p')
case "$image_id" in
    sha256:*) ;;
    *) fail "provenance verifier did not return one immutable image ID" ;;
esac
[ "$(printf '%s\n' "$image_id" | wc -l)" -eq 1 ] \
    || fail "provenance verifier returned ambiguous image identity"

help_output=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    "$image_id" --help 2>&1) \
    || fail "candidate gaudere-agent --help probe failed"
printf '%s\n' "$help_output" | grep -q -- '--local-goose-model' \
    || fail "candidate image predates local Goose service capability"

version_output=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    --entrypoint /usr/local/bin/goose \
    "$image_id" --version 2>&1) \
    || fail "candidate Goose version probe failed"
printf '%s\n' "$version_output" | grep -q '1.49.0' \
    || fail "candidate image does not contain pinned Goose 1.49.0"

rendered_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-local-goose-quadlet.XXXXXX")
previous_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-local-goose-previous.XXXXXX")
install -m 0600 "$target_quadlet" "$previous_quadlet"

cleanup()
{
    rm -f -- "$rendered_quadlet" "$previous_quadlet"
}
trap cleanup EXIT
trap 'cleanup; exit 1' HUP INT TERM

python3 - "$target_quadlet" "$rendered_quadlet" "$image_id" "$model_id" "$model_sha256" "$goose_root" "$model_container_root" "$governance_container_path" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
image_id, model_id, model_sha, host_root, model_root, governance = sys.argv[3:]
lines = source.read_text(encoding='utf-8').splitlines(keepends=True)
image_indexes = [i for i, line in enumerate(lines) if line.startswith('Image=')]
exec_indexes = [i for i, line in enumerate(lines) if line.startswith('Exec=')]
if len(image_indexes) != 1 or len(exec_indexes) != 1:
    raise SystemExit('cannot render ambiguous Quadlet')

i = image_indexes[0]
ending = '\n' if lines[i].endswith('\n') else ''
lines[i] = f'Image={image_id}{ending}'

e = exec_indexes[0]
ending = '\n' if lines[e].endswith('\n') else ''
base = lines[e].rstrip('\n')
selector = '/' + model_id
base += (
    f' --local-goose-model {selector}'
    f' --local-goose-model-sha256 {model_sha}'
    f' --local-goose-governance {governance}'
)
lines[e] = base + ending

volume_line = f'Volume={host_root}:{model_root}:ro,Z\n'
insert_at = max(i for i, line in enumerate(lines) if line.startswith('Volume=')) + 1
lines.insert(insert_at, volume_line)
rendered = ''.join(lines)

r_lines = rendered.splitlines()
r_exec = [line for line in r_lines if line.startswith('Exec=')]
if len(r_exec) != 1:
    raise SystemExit('rendered Quadlet has ambiguous Exec=')
if '--openai-model ' in r_exec[0] or '--autonomous-pulse-provider' in r_exec[0] or '--wake-intents' in r_exec[0]:
    raise SystemExit('rendered Quadlet unexpectedly grants external/provider authority')
if f'--local-goose-model {selector}' not in r_exec[0]:
    raise SystemExit('rendered Quadlet lost local Goose model selector')
if 'Network=none' not in r_lines:
    raise SystemExit('rendered Quadlet lost Network=none')
if volume_line.rstrip('\n') not in r_lines:
    raise SystemExit('rendered Quadlet lost read-only Goose model mount')
destination.write_text(rendered, encoding='utf-8')
PY

# Mechanically allow only: Image= replacement, Exec= local-Goose suffix and one
# new read-only model Volume=. Everything else must remain byte-identical.
python3 - "$target_quadlet" "$rendered_quadlet" "$goose_root" "$model_container_root" <<'PY'
import pathlib
import sys

old = pathlib.Path(sys.argv[1]).read_text(encoding='utf-8').splitlines()
new = pathlib.Path(sys.argv[2]).read_text(encoding='utf-8').splitlines()
volume = f'Volume={sys.argv[3]}:{sys.argv[4]}:ro,Z'

def normalize(lines, rendered):
    result = []
    for line in lines:
        if line.startswith('Image='):
            result.append('Image=<allowed>')
        elif line.startswith('Exec=') and rendered:
            marker = ' --local-goose-model '
            result.append(line.split(marker, 1)[0] if marker in line else line)
        elif rendered and line == volume:
            continue
        else:
            result.append(line)
    return result

if normalize(old, False) != normalize(new, True):
    raise SystemExit('local Goose promotion attempted an unauthorized Quadlet mutation')
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

printf 'gaudere local-goose provider-off promotion: quadlet=%s\n' "$target_quadlet"
printf 'gaudere local-goose provider-off promotion: runtime_image_id=%s\n' "$image_id"
printf 'gaudere local-goose provider-off promotion: model_id=%s\n' "$model_id"
printf 'gaudere local-goose provider-off promotion: model_sha256=%s\n' "$model_sha256"
printf 'gaudere local-goose provider-off promotion: model_mount=read-only\n'
printf 'gaudere local-goose provider-off promotion: provider_authority=OFF\n'
printf 'gaudere local-goose provider-off promotion: network=none\n'
printf 'gaudere local-goose provider-off promotion: service remains stopped\n'
