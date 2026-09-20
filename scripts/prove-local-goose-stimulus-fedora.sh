#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
production_stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
proof_root="$data_home/gaudere/.local-goose-stimulus-proof"
container_name="gaudere-local-goose-stimulus-proof-$$"
workspace=""
proof_tag=""
proof_container_started=0
proof_image_built=0

fail()
{
    printf 'gaudere Local Goose stimulus Fedora proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$proof_container_started" = "1" ]; then
        "$podman_command" rm -f "$container_name" >/dev/null 2>&1 || true
    fi
    if [ "$proof_image_built" = "1" ] && [ -n "$proof_tag" ]; then
        "$podman_command" image rm "$proof_tag" >/dev/null 2>&1 || true
    fi
    if [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git sqlite3 chmod mkdir mktemp rm grep awk cut sed tr sleep; do
    command -v "$command" >/dev/null 2>&1         || fail "required command not found: $command"
done

[ -f "$state_database" ] && [ ! -L "$state_database" ]     || fail "production state database is missing or unsafe"
[ -f "$cycle_sidecar" ] && [ ! -L "$cycle_sidecar" ]     || fail "production Local Goose cycle sidecar is missing or unsafe"
[ ! -e "$production_stimulus_sidecar" ]     || fail "production stimulus sidecar already exists; isolated proof must not reuse it"

[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ]     || fail "script must belong to the gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ]     || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ]     || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
short_ref=$(printf '%s\n' "$agent_ref" | cut -c1-12)
proof_tag="localhost/gaudere-agent:stimulus-proof-$short_ref"

case "$agent_ref" in
    *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal SHA" ;;
esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"

service_before=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
[ "$service_before" = "active" ] || fail "production service is not active"

image_before=$("$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null     | sed 's/^sha256://')
[ -n "$image_before" ] || fail "cannot resolve production image"

network_before=$("$podman_command" inspect gaudere-agent     --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_before" = "none" ] || fail "production container network is not none"

provider_before=$(sqlite3 -readonly "$state_database"     "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$provider_before" = "$expected_provider_total" ]     || fail "production provider total is $provider_before, expected $expected_provider_total"

cursor_query="SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';"
production_cursor_before=$(sqlite3 -readonly "$cycle_sidecar" "$cursor_query")
[ -n "$production_cursor_before" ] || fail "production Local Goose cursor is missing"
production_revision=$(printf '%s\n' "$production_cursor_before" | awk -F'|' '{print $1}')
production_generation=$(printf '%s\n' "$production_cursor_before" | awk -F'|' '{print $2}')
production_state=$(printf '%s\n' "$production_cursor_before" | awk -F'|' '{print $3}')
[ "$production_state" = "0" ] || fail "production Local Goose cycle is not dormant"
case "$production_generation" in
    ''|*[!0-9]*) fail "production Local Goose generation is invalid" ;;
esac
[ "$production_generation" -ge 2 ] || fail "production Local Goose generation is below 2"

production_cycle_tasks_before=$(sqlite3 -readonly "$state_database"     "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")

mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/run.XXXXXX")
state_copy="$workspace/state.db"
cycle_copy="$workspace/local-goose-cycle.db"
stimulus_copy="$workspace/local-goose-cycle-stimulus.db"
socket_copy="$workspace/control.sock"

sqlite3 -readonly "$state_database" ".backup '$state_copy'"
sqlite3 -readonly "$cycle_sidecar" ".backup '$cycle_copy'"
chmod 0600 "$state_copy" "$cycle_copy"

copy_cursor_before=$(sqlite3 -readonly "$cycle_copy" "$cursor_query")
[ "$copy_cursor_before" = "$production_cursor_before" ]     || fail "copied Local Goose cursor differs from production snapshot"
copy_provider_before=$(sqlite3 -readonly "$state_copy"     "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$copy_provider_before" = "$provider_before" ]     || fail "copied provider total differs from production snapshot"
copy_cycle_tasks_before=$(sqlite3 -readonly "$state_copy"     "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
[ "$copy_cycle_tasks_before" = "$production_cycle_tasks_before" ]     || fail "copied Local Goose Task count differs from production snapshot"

printf '=== BUILD DISPOSABLE FEDORA 44 PROOF IMAGE ===\n'
"$podman_command" build     --target builder     --build-arg BUILDER_IMAGE=registry.fedoraproject.org/fedora:44     --build-arg "GAUDERE_AGENT_REF=$agent_ref"     --build-arg "GAUDERE_REF=$core_ref"     --tag "$proof_tag"     --file "$repository_root/Containerfile"     "$repository_root"
proof_image_built=1

"$podman_command" run --rm --network none     --entrypoint /usr/bin/test "$proof_tag"     -x /opt/gaudere-agent/bin/gaudere-local-goose-cycle-stimulus-proof     || fail "proof image lacks isolated stimulus proof runtime"

printf '=== RUN ISOLATED NETWORK-NONE LIVE-CONTROL PROOF ===\n'
"$podman_command" run -d     --name "$container_name"     --network none     --userns=keep-id     --read-only     --read-only-tmpfs     --security-opt=no-new-privileges     --cap-drop=all     --env LD_LIBRARY_PATH=/opt/gaudere/lib     --volume "$workspace:/proof:Z"     --entrypoint /opt/gaudere-agent/bin/gaudere-local-goose-cycle-stimulus-proof     "$proof_tag"     --state /proof/state.db     --cycle-sidecar /proof/local-goose-cycle.db     --stimulus-sidecar /proof/local-goose-cycle-stimulus.db     --control-socket /proof/control.sock >/dev/null
proof_container_started=1

proof_network=$("$podman_command" inspect "$container_name"     --format '{{.HostConfig.NetworkMode}}')
[ "$proof_network" = "none" ] || fail "proof container network is not none"

attempt=0
while [ "$attempt" -lt 100 ] && [ ! -S "$socket_copy" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
done
[ -S "$socket_copy" ] || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "proof control socket did not become ready"
}

request_id="fedora-proof-$short_ref"
client_output=$("$podman_command" exec     --env LD_LIBRARY_PATH=/opt/gaudere/lib     "$container_name"     /opt/gaudere-agent/bin/gaudere-control     --socket /proof/control.sock     stimulate-local-goose-cycle "$request_id")     || {
        "$podman_command" logs "$container_name" >&2 || true
        fail "bounded stimulus live-control request failed"
    }

container_exit=$("$podman_command" wait "$container_name")
[ "$container_exit" = "0" ] || {
    "$podman_command" logs "$container_name" >&2 || true
    fail "proof runtime exited with status $container_exit"
}

server_output=$("$podman_command" logs "$container_name")
printf '%s\n' "$client_output"
printf '%s\n' "$server_output"
printf '%s\n' "$client_output" | grep -q '^acceptance=accepted$'     || fail "live-control stimulus was not newly accepted"
printf '%s\n' "$client_output" | grep -q '^result=consumed$'     || fail "live-control stimulus was not consumed"
printf '%s\n' "$client_output" | grep -q '^cycle_state=scheduled$'     || fail "copied cycle did not become scheduled"
printf '%s\n' "$server_output" | grep -q '^FEDORA_LOCAL_GOOSE_STIMULUS_CONTROL=PASS$'     || fail "proof runtime did not report PASS"

copy_cursor_after=$(sqlite3 -readonly "$cycle_copy" "$cursor_query")
copy_revision_after=$(printf '%s\n' "$copy_cursor_after" | awk -F'|' '{print $1}')
copy_generation_after=$(printf '%s\n' "$copy_cursor_after" | awk -F'|' '{print $2}')
copy_state_after=$(printf '%s\n' "$copy_cursor_after" | awk -F'|' '{print $3}')
copy_due_after=$(printf '%s\n' "$copy_cursor_after" | awk -F'|' '{print $4}')
expected_revision=$((production_revision + 1))

[ "$copy_revision_after" = "$expected_revision" ]     || fail "copied cycle revision did not advance exactly once"
[ "$copy_generation_after" = "$production_generation" ]     || fail "stimulus incorrectly changed copied cycle generation"
[ "$copy_state_after" = "1" ]     || fail "copied cycle is not scheduled after stimulus"
[ -n "$copy_due_after" ]     || fail "copied scheduled cycle lacks due_at_ms"

stimulus_count=$(sqlite3 -readonly "$stimulus_copy"     "SELECT COUNT(*) FROM local_goose_cycle_stimuli;")
[ "$stimulus_count" = "1" ] || fail "proof stimulus ledger does not contain exactly one record"
stimulus_row=$(sqlite3 -readonly "$stimulus_copy"     "SELECT source_id||'|'||status||'|'||target_cycle_revision||'|'||target_cycle_generation||'|'||COALESCE(resulting_cycle_revision,'') FROM local_goose_cycle_stimuli;")
stimulus_source=$(printf '%s\n' "$stimulus_row" | awk -F'|' '{print $1}')
stimulus_status=$(printf '%s\n' "$stimulus_row" | awk -F'|' '{print $2}')
stimulus_target_revision=$(printf '%s\n' "$stimulus_row" | awk -F'|' '{print $3}')
stimulus_target_generation=$(printf '%s\n' "$stimulus_row" | awk -F'|' '{print $4}')
stimulus_result_revision=$(printf '%s\n' "$stimulus_row" | awk -F'|' '{print $5}')

[ "$stimulus_source" = "$request_id" ] || fail "stimulus source identity differs"
[ "$stimulus_status" = "1" ] || fail "stimulus did not settle consumed"
[ "$stimulus_target_revision" = "$production_revision" ]     || fail "stimulus targeted wrong cycle revision"
[ "$stimulus_target_generation" = "$production_generation" ]     || fail "stimulus targeted wrong cycle generation"
[ "$stimulus_result_revision" = "$expected_revision" ]     || fail "stimulus recorded wrong resulting revision"

copy_provider_after=$(sqlite3 -readonly "$state_copy"     "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$copy_provider_after" = "$copy_provider_before" ]     || fail "copied provider total changed during isolated proof"
copy_cycle_tasks_after=$(sqlite3 -readonly "$state_copy"     "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
[ "$copy_cycle_tasks_after" = "$copy_cycle_tasks_before" ]     || fail "isolated stimulus proof created or replayed a Local Goose Task"

service_after=$("$systemctl_command" --user is-active "$service_name" 2>/dev/null || true)
[ "$service_after" = "$service_before" ] || fail "production service state changed"
image_after=$("$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null     | sed 's/^sha256://')
[ "$image_after" = "$image_before" ] || fail "production image changed"
network_after=$("$podman_command" inspect gaudere-agent     --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)
[ "$network_after" = "$network_before" ] || fail "production network mode changed"
provider_after=$(sqlite3 -readonly "$state_database"     "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';")
[ "$provider_after" = "$provider_before" ] || fail "production provider total changed"
production_cursor_after=$(sqlite3 -readonly "$cycle_sidecar" "$cursor_query")
[ "$production_cursor_after" = "$production_cursor_before" ]     || fail "production Local Goose cursor changed during isolated proof"
production_cycle_tasks_after=$(sqlite3 -readonly "$state_database"     "SELECT COUNT(*) FROM tasks WHERE kind='cognition.local-goose-cycle.v1';")
[ "$production_cycle_tasks_after" = "$production_cycle_tasks_before" ]     || fail "production Local Goose Task set changed during isolated proof"
[ ! -e "$production_stimulus_sidecar" ]     || fail "isolated proof created a production stimulus sidecar"

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PROOF_NETWORK=%s\n' "$proof_network"
printf 'PRODUCTION_IMAGE=%s\n' "$image_after"
printf 'PRODUCTION_NETWORK=%s\n' "$network_after"
printf 'PROVIDER_TOTAL=%s\n' "$provider_after"
printf 'PRODUCTION_CURSOR=%s\n' "$production_cursor_after"
printf 'COPIED_CURSOR_BEFORE=%s\n' "$copy_cursor_before"
printf 'COPIED_CURSOR_AFTER=%s\n' "$copy_cursor_after"
printf 'STIMULUS=%s\n' "$stimulus_row"
printf 'LOCAL_GOOSE_TASKS=%s\n' "$production_cycle_tasks_after"
printf 'FEDORA_LOCAL_GOOSE_STIMULUS_ISOLATED=PASS\n'
