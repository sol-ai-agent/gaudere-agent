#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
secret_name=${GAUDERE_OPENAI_SECRET_NAME:-gaudere-openai-api-key}
image=${GAUDERE_IMAGE:-localhost/gaudere-agent:dev}
quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
source_quadlet=deploy/quadlet/gaudere-agent-openai.container.in
target_quadlet="$quadlet_directory/gaudere-agent.container"

fail()
{
    printf 'gaudere OpenAI service install: %s\n' "$*" >&2
    exit 1
}

normalize_image_id()
{
    value=$1
    case "$value" in
        sha256:*) digest=${value#sha256:} ;;
        *) digest=$value ;;
    esac
    case "$digest" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
    [ "${#digest}" -eq 64 ] || return 1
    printf 'sha256:%s\n' "$digest"
}

for command in "$podman_command" systemctl python3 flock install mktemp rm; do
    command -v "$command" >/dev/null 2>&1 \
        || fail "required command not found: $command"
done
[ -f "$source_quadlet" ] || fail "OpenAI Quadlet template not found: $source_quadlet"
[ -f "$state_database" ] || fail "production state database not found: $state_database"

service_state=$(systemctl --user is-active "$service_name" 2>/dev/null || true)
case "$service_state" in
    active|activating|reloading)
        fail "$service_name must be stopped before installing provider capability"
        ;;
esac

# Hold the same state lock used by gaudere-agent while inspecting the stopped DB and
# replacing the service configuration. This prevents a second owner from appearing
# between the activation fence and Quadlet installation.
exec 9>>"$state_database.lock"
chmod 600 "$state_database.lock" 2>/dev/null || true
flock -n 9 || fail "state database is currently owned"

schema=$(python3 - "$state_database" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    print(db.execute("PRAGMA user_version").fetchone()[0])
PY
)
case "$schema" in
    3|4)
        ;;
    *)
        fail "provider capability requires production schema v3 or v4 (found $schema)"
        ;;
esac

# Capability activation must never turn inherited provider work into an external
# call merely because its handler became available. Refuse every nonterminal task
# kind that uses the OpenAI provider, including bounded reflection. Also refuse any
# active task of another kind: a clean stopped-state transition should have no leased
# work at all.
provider_blocker=$(python3 - "$state_database" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    row = db.execute(
        "SELECT id,status FROM tasks "
        "WHERE kind IN ('provider.openai.responses','cognition.reflect.v1') "
        "AND status IN (0,1,2) "
        "ORDER BY rowid LIMIT 1"
    ).fetchone()
    if row:
        print(f"{row[0]}:{row[1]}")
PY
)
[ -z "$provider_blocker" ] \
    || fail "pre-existing nonterminal provider task requires review before activation: $provider_blocker"

active_blocker=$(python3 - "$state_database" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    row = db.execute(
        "SELECT id,kind,status FROM tasks WHERE status IN (1,2) ORDER BY rowid LIMIT 1"
    ).fetchone()
    if row:
        print(f"{row[0]}:{row[1]}:{row[2]}")
PY
)
[ -z "$active_blocker" ] \
    || fail "active durable work remains in stopped state: $active_blocker"

"$podman_command" image exists "$image" \
    || fail "runtime image does not exist: $image"
observed_image_id=$("$podman_command" image inspect --format '{{.Id}}' "$image" 2>/dev/null) \
    || fail "cannot resolve runtime image ID: $image"
image_id=$(normalize_image_id "$observed_image_id") \
    || fail "runtime image did not resolve to one full sha256 ID: $image"

# From this point onward use the immutable image ID only. A mutable tag may be the
# operator-friendly input, but it cannot drift between validation, compatibility
# probing, and the installed service profile.
image="$image_id"

# Refuse to install an older image that can expose the raw provider but cannot
# accept the bounded-reflection command documented by this profile. This runs the
# control binary only, with networking disabled and without mounting any secret or
# production state.
control_usage=$("$podman_command" run --rm \
    --network none \
    --read-only \
    --cap-drop=all \
    --security-opt=no-new-privileges \
    --entrypoint /usr/local/bin/gaudere-control \
    "$image" 2>&1 || true)
printf '%s\n' "$control_usage" | grep -q 'reflect ID OBJECTIVE' \
    || fail "runtime image predates bounded reflection; rebuild current main first"

# Schema v4 support is a recovery compatibility gate, not WakeIntent activation.
# Prove that the selected immutable image can reopen a consistent disposable copy
# through its default path. The probe has no network, secret, provider configuration,
# or --wake-intents flag and never mounts the production state directory.
if [ "$schema" = "4" ]; then
    compatibility_root=${TMPDIR:-/tmp}
    compatibility_prefix="$compatibility_root/gaudere-openai-schema-v4."
    compatibility_directory=

    cleanup_compatibility()
    {
        case "$compatibility_directory" in
            "")
                ;;
            "$compatibility_prefix"*)
                rm -rf -- "$compatibility_directory"
                compatibility_directory=
                ;;
            *)
                printf 'gaudere OpenAI service install: refusing unsafe temporary cleanup path: %s\n' \
                    "$compatibility_directory" >&2
                ;;
        esac
    }

    trap cleanup_compatibility EXIT
    trap 'cleanup_compatibility; exit 1' HUP INT TERM
    compatibility_directory=$(mktemp -d "${compatibility_prefix}XXXXXX")
    compatibility_database="$compatibility_directory/state.db"

    python3 - "$state_database" "$compatibility_database" <<'PY'
import pathlib
import sqlite3
import sys

source_uri = pathlib.Path(sys.argv[1]).resolve().as_uri() + "?mode=ro"
with sqlite3.connect(source_uri, uri=True) as source:
    source.execute("PRAGMA query_only=ON")
    with sqlite3.connect(sys.argv[2]) as target:
        source.backup(target)
        if target.execute("PRAGMA user_version").fetchone()[0] != 4:
            raise SystemExit("disposable compatibility copy is not schema v4")
        if target.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            raise SystemExit("disposable compatibility copy failed integrity_check")
PY

    if ! compatibility_output=$("$podman_command" run --rm \
        --network none \
        --userns keep-id \
        --read-only \
        --read-only-tmpfs=true \
        --cap-drop=all \
        --security-opt=no-new-privileges \
        --pids-limit 64 \
        --memory 256m \
        --volume "$compatibility_directory:/var/lib/gaudere:Z" \
        "$image" --state /var/lib/gaudere/state.db --check 2>&1); then
        printf '%s\n' "$compatibility_output" >&2
        fail "runtime image cannot safely reopen schema v4 without --wake-intents"
    fi
    printf '%s\n' "$compatibility_output" | grep -q '^gaudere-agent: safe$' \
        || fail "runtime image schema-v4 compatibility probe did not finish safe"
    if printf '%s\n' "$compatibility_output" | grep -q 'explicit wake enabled'; then
        fail "runtime image schema-v4 compatibility probe enabled WakeIntent"
    fi

    python3 - "$compatibility_database" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as db:
    if db.execute("PRAGMA user_version").fetchone()[0] != 4:
        raise SystemExit("runtime image changed the disposable schema version")
    if db.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
        raise SystemExit("runtime image left the disposable copy inconsistent")
PY

    cleanup_compatibility
    trap - EXIT HUP INT TERM
    printf 'gaudere OpenAI service install: schema v4 default reopen probe passed without WakeIntent\n'
fi

"$podman_command" secret exists "$secret_name" \
    || fail "Podman secret $secret_name does not exist; install it with scripts/install-openai-secret.sh"

install -d -m 0700 "$quadlet_directory" "$state_directory"
rendered_quadlet=$(mktemp "${TMPDIR:-/tmp}/gaudere-openai-quadlet.XXXXXX")
cleanup_rendered()
{
    rm -f -- "$rendered_quadlet"
}
trap cleanup_rendered EXIT HUP INT TERM
python3 - "$source_quadlet" "$rendered_quadlet" "$image_id" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
image_id = sys.argv[3]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
indexes = [i for i, line in enumerate(lines) if line.startswith("Image=")]
if len(indexes) != 1:
    raise SystemExit("OpenAI Quadlet template must contain exactly one Image= line")
index = indexes[0]
ending = "\n" if lines[index].endswith("\n") else ""
lines[index] = f"Image={image_id}{ending}"
destination.write_text("".join(lines), encoding="utf-8")
PY
install -m 0600 "$rendered_quadlet" "$target_quadlet"
cleanup_rendered
trap - EXIT HUP INT TERM
systemctl --user daemon-reload

printf 'gaudere OpenAI service install: installed capability profile as %s\n' "$target_quadlet"
printf 'gaudere OpenAI service install: runtime_image_id=%s\n' "$image_id"
printf 'gaudere OpenAI service install: model=gpt-5.6-sol secret=%s\n' "$secret_name"
printf 'gaudere OpenAI service install: no provider task was submitted and the service remains stopped\n'
printf 'gaudere OpenAI service install: revert with scripts/install-user-service.sh while the service is stopped\n'
printf 'Start explicitly with: systemctl --user start %s\n' "$service_name"
