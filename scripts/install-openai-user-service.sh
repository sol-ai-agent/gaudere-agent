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

for command in "$podman_command" systemctl python3 flock install; do
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
[ "$schema" = "3" ] || fail "provider capability requires production schema v3 (found $schema)"

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

"$podman_command" secret exists "$secret_name" \
    || fail "Podman secret $secret_name does not exist; install it with scripts/install-openai-secret.sh"

install -d -m 0700 "$quadlet_directory" "$state_directory"
install -m 0600 "$source_quadlet" "$target_quadlet"
systemctl --user daemon-reload

printf 'gaudere OpenAI service install: installed capability profile as %s\n' "$target_quadlet"
printf 'gaudere OpenAI service install: model=gpt-5.6-sol secret=%s\n' "$secret_name"
printf 'gaudere OpenAI service install: no provider task was submitted and the service remains stopped\n'
printf 'gaudere OpenAI service install: revert with scripts/install-user-service.sh while the service is stopped\n'
printf 'Start explicitly with: systemctl --user start %s\n' "$service_name"
