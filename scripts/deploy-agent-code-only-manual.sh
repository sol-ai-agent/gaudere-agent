#!/usr/bin/env bash
# Gaudere agent: manual code-only Fedora release with rollback.
# Does not touch B10; does not invoke any provider, Task, ACK or dialogue turn.
# By default performs a non-disruptive preflight; --apply requires an explicit token.
set -Eeuo pipefail
umask 077

# Source-only tool: no hardcoded old/new image or release branch.
# Pin these four values from independently verified Fedora and GitHub evidence.
OLD_IMAGE=${GAUDERE_MANUAL_OLD_IMAGE:?set immutable old sha256 image ID}
IMAGE=${GAUDERE_MANUAL_NEW_IMAGE:?set immutable candidate sha256 image ID}
AGENT=${GAUDERE_MANUAL_AGENT_REF:?set exact agent Git commit}
CORE=${GAUDERE_MANUAL_CORE_REF:?set exact core Git commit}
CHECKOUT=${GAUDERE_MANUAL_CHECKOUT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
OLD=${OLD_IMAGE#sha256:}
NEW=${IMAGE#sha256:}
UNIT=gaudere-agent.service
QUADLET="$HOME/.config/containers/systemd/gaudere-agent.container"
STATE="$HOME/.local/share/gaudere/state"
BACKUP_TOOL="$CHECKOUT/scripts/backup-state.sh"
VERIFY_TOOL="$CHECKOUT/scripts/verify-image-provenance.sh"
WORK_ROOT="$HOME/.local/share/gaudere/manual-releases"
WORK=""
BACKUP=""
NEXT_QUADLET=""
STAGING_CREATED=0
ARMED=0
NEW_STARTED=0
COMMITTED=0
APPLY=0
case "${1:-}" in
    '') ;;
    --apply) APPLY=1 ;;
    *) printf 'usage: %s [--apply]\n' "$0" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { printf 'unexpected arguments\n' >&2; exit 2; }
if [[ "$APPLY" == 1 ]]; then
    [[ "${GAUDERE_MANUAL_RELEASE_ACK:-}" == APPLY_GAUDERE_AGENT_CODE_ONLY ]] || {
        printf 'manual release requires an explicit operator approval token\n' >&2
        exit 2
    }
fi
for ref in "$OLD_IMAGE" "$IMAGE"; do
    [[ "$ref" =~ ^sha256:[0-9a-f]{64}$ ]] || { printf 'invalid pinned image id\n' >&2; exit 2; }
done
for ref in "$AGENT" "$CORE"; do
    [[ "$ref" =~ ^[0-9a-f]{40}$ ]] || { printf 'invalid pinned Git ref\n' >&2; exit 2; }
done
[[ "$OLD_IMAGE" != "$IMAGE" ]] || { printf 'old and new images are identical\n' >&2; exit 2; }

fail() { printf 'RELEASE=FAIL: %s\n' "$*" >&2; exit 1; }
service_active() { [[ "$(systemctl --user is-active "$UNIT" 2>/dev/null || :)" == active ]]; }
running_image() { podman inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://'; }
state_signature() {
    local file name digest
    shopt -s nullglob
    for file in "$STATE"/*.db; do
        name=${file##*/}
        digest=$(sqlite3 -readonly "$file" .dump | sha256sum | cut -d' ' -f1) || return 1
        printf '%s %s\n' "$name" "$digest"
    done | LC_ALL=C sort
}

on_exit() {
    local code=$? restored=1
    trap - EXIT INT TERM
    if [[ "$COMMITTED" == 1 ]]; then
        if [[ "$APPLY" == 1 ]]; then
            printf 'RELEASE=PASS\nNEW_IMAGE=sha256:%s\nBACKUP=%s\nROLLBACK_IMAGE=sha256:%s\n' "$NEW" "$BACKUP" "$OLD"
        fi
        return 0
    fi
    if [[ "$STAGING_CREATED" == 1 && -n "$NEXT_QUADLET" ]]; then
        rm -f -- "$NEXT_QUADLET" 2>/dev/null || :
    fi
    if [[ "$ARMED" == 0 ]]; then
        printf 'RELEASE=BLOCKED_BEFORE_STOP\n' >&2
        return "$code"
    fi
    printf 'RELEASE=ROLLBACK_ATTEMPT (reason exit=%s)\n' "$code" >&2
    systemctl --user stop "$UNIT" || restored=0
    # A successful stop command alone does not prove that the candidate is
    # gone. Never restore its mounted SQLite state while it may still run.
    local container_state exists_rc=0
    [[ "$(systemctl --user is-active "$UNIT" 2>/dev/null || :)" == inactive ]] || restored=0
    if [[ "$restored" == 1 ]]; then
        if container_state=$(podman inspect gaudere-agent --format '{{.State.Status}}' 2>/dev/null); then
            case "$container_state" in
                exited|stopped) ;;
                *) restored=0 ;;
            esac
        else
            # A Quadlet may remove the stopped container. Only the exact
            # exit code 1 proves absence; 125 denotes a Podman store error.
            podman container exists gaudere-agent >/dev/null 2>&1 || exists_rc=$?
            [[ "$exists_rc" == 1 ]] || restored=0
        fi
    fi
    if [[ "$restored" != 1 ]]; then
        printf 'ROLLBACK=MANUAL_REVIEW_REQUIRED; candidate stop unverified, Quadlet and state left intact: %s\n' "$WORK" >&2
        return 1
    fi
    if [[ -n "$WORK" && -f "$WORK/quadlet.before" ]]; then
        install -m 0600 "$WORK/quadlet.before" "$QUADLET" || restored=0
        systemctl --user daemon-reload || restored=0
    else
        restored=0
    fi
    if [[ "$NEW_STARTED" == 1 && -n "$BACKUP" && -f "$BACKUP" && "$restored" == 1 ]]; then
        if [[ -d "$STATE" && ! -e "$WORK/state.failed" ]]; then
            mv "$STATE" "$WORK/state.failed" || restored=0
        else
            restored=0
        fi
        if [[ "$restored" == 1 ]]; then
            mkdir -m 0700 "$STATE" && tar -xzf "$BACKUP" -C "$STATE" || restored=0
        fi
    fi
    if [[ "$restored" == 1 ]]; then
        systemctl --user start "$UNIT" || restored=0
        service_active || restored=0
        [[ "$(running_image)" == "$OLD" ]] || restored=0
    fi
    if [[ "$restored" == 1 ]]; then
        printf 'ROLLBACK=PASS old image active; inspect %s\n' "$WORK" >&2
    else
        printf 'ROLLBACK=MANUAL_REVIEW_REQUIRED; leave Fedora state/backup intact: %s\n' "$WORK" >&2
    fi
    return 1
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for tool in podman systemctl git python3 sqlite3 sha256sum install tar mktemp sed grep cut flock stat cmp; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing executable $tool"
done
[[ -f "$QUADLET" && ! -L "$QUADLET" ]] || fail 'Quadlet absent or symlink'
[[ "$(stat -c %a "$QUADLET")" == 600 ]] || fail 'Quadlet mode not 0600'
[[ -f "$STATE/state.db" && -d "$STATE" ]] || fail 'state database missing'
[[ -f "$BACKUP_TOOL" && -f "$VERIFY_TOOL" ]] || fail 'backup or provenance tool absent'
[[ "$(git -C "$CHECKOUT" rev-parse HEAD)" == "$AGENT" ]] || fail 'release checkout not pinned to target'
[[ -z "$(git -C "$CHECKOUT" status --porcelain --untracked-files=normal)" ]] || fail 'release checkout dirty'
[[ "$(tr -d '\r\n' < "$CHECKOUT/gaudere.ref")" == "$CORE" ]] || fail 'core ref mismatch'
service_active || fail 'production unit not active at preflight'
[[ "$(running_image)" == "$OLD" ]] || fail 'running image differs from expected rollback image'
[[ "$(podman inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}')" == none ]] || fail 'production network differs from none'
[[ "$(grep -c '^Image=' "$QUADLET")" == 1 ]] || fail 'Quadlet must have one Image='
grep -Fxq "Image=$OLD_IMAGE" "$QUADLET" || fail 'Quadlet Image= differs from expected old image'
grep -Fxq 'Network=none' "$QUADLET" || fail 'Quadlet must have Network=none'
grep -Fxq 'Volume=%h/.local/share/gaudere/state:/var/lib/gaudere:Z' "$QUADLET" || fail 'state volume differs from known production'
if grep -Eq -- '--openai-model|--autonomous-pulse-provider|--wake-intents' "$QUADLET"; then
    fail 'provider authority unexpectedly present; refuse code-only release'
fi
[[ "$(podman image inspect "$IMAGE" --format '{{.Id}}' | sed 's/^sha256://')" == "$NEW" ]] || fail 'candidate image id differs'
PODMAN=podman sh "$VERIFY_TOOL" "$IMAGE" "$AGENT" "$CORE" "$IMAGE"

# A provider-free, no-volume image probe. Nothing in production is mounted.
# gaudere-chat --help prints its one-line usage on stderr, even on success.
# The help does not enumerate interactive slash commands such as /retry.
CHAT_HELP=$(podman run --rm --network none --read-only --read-only-tmpfs \
    --security-opt=no-new-privileges --cap-drop=all \
    --entrypoint /usr/local/bin/gaudere-chat "$IMAGE" --help 2>&1) || fail 'candidate help failed'
printf '%s\n' "$CHAT_HELP"
[[ "$CHAT_HELP" == *"Usage:"* ]] || fail 'gaudere-chat usage is absent'
[[ "$CHAT_HELP" == *"--io-timeout-ms"* ]] || fail 'gaudere-chat I/O timeout flag is absent'

printf 'PRECHECK=PASS old=%s candidate=%s\n' "$OLD" "$NEW"
if [[ "$APPLY" == 0 ]]; then
    COMMITTED=1
    printf 'RELEASE=PRECHECK_PASS\n'
    exit 0
fi

mkdir -p -m 0700 "$WORK_ROOT"
[[ -d "$WORK_ROOT" && ! -L "$WORK_ROOT" && "$(stat -c %a "$WORK_ROOT")" == 700 ]] || fail 'workspace root is unsafe'
[[ ! -L "$WORK_ROOT/.release.lock" ]] || fail 'release lock symlink is unsafe'
exec 9>> "$WORK_ROOT/.release.lock"
chmod 0600 "$WORK_ROOT/.release.lock"
flock -n 9 || fail 'another manual release is active'
WORK=$(mktemp -d "$WORK_ROOT/release.XXXXXXXX")
# Revalidate the current host state after taking the exclusive release lock.
# Nothing is stopped if another operator changed the running image in between.
service_active || fail 'agent stopped between preflight and release lock'
[[ "$(running_image)" == "$OLD" ]] || fail 'running image changed before release lock'
grep -Fxq "Image=$OLD_IMAGE" "$QUADLET" || fail 'Quadlet changed before release lock'
install -m 0600 "$QUADLET" "$WORK/quadlet.before"
# mktemp atomically claims a unique staged path; never remove another
# operator's pre-existing predictable .tmp file.
NEXT_QUADLET=$(mktemp "$QUADLET.manual-release.XXXXXXXX")
STAGING_CREATED=1
python3 - "$QUADLET" "$NEXT_QUADLET" "$OLD_IMAGE" "$IMAGE" <<'PY'
from pathlib import Path
import os, sys
source, dest = map(Path, sys.argv[1:3])
old, new = sys.argv[3:]
text = source.read_text(encoding='utf-8')
if text.count('Image=') != 1 or ('Image=' + old + '\n') not in text:
    raise SystemExit('unexpected original Quadlet Image=')
replacement = text.replace('Image=' + old + '\n', 'Image=' + new + '\n', 1)
if replacement.replace('Image=' + new + '\n', 'Image=' + old + '\n', 1) != text:
    raise SystemExit('Quadlet mutation beyond Image=')
with dest.open('w', encoding='utf-8') as fh:
    fh.write(replacement)
os.chmod(dest, 0o600)
PY

printf 'SERVICE_STOP=BEGIN\n'
ARMED=1
systemctl --user stop "$UNIT"
[[ "$(systemctl --user is-active "$UNIT" 2>/dev/null || :)" == inactive ]] || fail 'unit not fully stopped'
[[ "$(podman inspect gaudere-agent --format '{{.State.Status}}' 2>/dev/null || :)" != running ]] || fail 'old container still running'

BACKUP=$(GAUDERE_STATE_DIR="$STATE" sh "$BACKUP_TOOL") || fail 'locked backup failed'
[[ -f "$BACKUP" && -f "$BACKUP.sha256" ]] || fail 'backup archive/hash absent'
(cd "$(dirname "$BACKUP")" && sha256sum -c "$(basename "$BACKUP").sha256") || fail 'backup checksum failed'
state_signature > "$WORK/state.before.sha256"
[[ -s "$WORK/state.before.sha256" ]] || fail 'state signature empty'

# Only the Image= line changes, using an atomic rename in the same directory.
mv "$NEXT_QUADLET" "$QUADLET"
STAGING_CREATED=0
systemctl --user daemon-reload
NEW_STARTED=1
systemctl --user start "$UNIT"
service_active || fail 'new unit not active'
[[ "$(running_image)" == "$NEW" ]] || fail 'running image does not match candidate'
[[ "$(podman inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}')" == none ]] || fail 'network isolation changed'
sleep 3
service_active || fail 'unit stopped during stability check'
[[ "$(running_image)" == "$NEW" ]] || fail 'container image changed during stability check'

# No new provider calls, Task, durable sidecar, or dialogue state mutations permitted.
state_signature > "$WORK/state.after.sha256"
cmp -s "$WORK/state.before.sha256" "$WORK/state.after.sha256" || fail 'SQLite logical state changed on code-only update'
[[ "$(grep -c '^Image=' "$QUADLET")" == 1 ]] || fail 'Quadlet now malformed'
grep -Fxq "Image=$IMAGE" "$QUADLET" || fail 'new Quadlet image missing'
podman exec gaudere-agent /usr/local/bin/gaudere-chat --help >/dev/null 2>&1 || fail 'production chat help failed'
state_signature > "$WORK/state.final.sha256"
cmp -s "$WORK/state.before.sha256" "$WORK/state.final.sha256" || fail 'state changed during help probe'

COMMITTED=1
printf 'RELEASE_PROVENANCE=PASS\n'
