#!/bin/sh
set -eu

authorization=${GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_SCHEMA3_MIGRATION_AUTHORIZATION:-}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
consumer_id=system-responder-v1
thread_alias=main

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
state_directory=${GAUDERE_STATE_DIR:-"$data_home/gaudere/state"}
state_database="$state_directory/state.db"
cycle_sidecar="$state_directory/local-goose-cycle.db"
stimulus_sidecar="$state_directory/local-goose-cycle-stimulus.db"
thread_sidecar="$state_directory/local-goose-dialogue-thread.db"
completion_sidecar="$state_directory/local-goose-dialogue-completion.db"
responder_sidecar="$state_directory/local-goose-dialogue-responder.db"
backup_script="$script_directory/backup-state.sh"
migrator_containerfile="$repository_root/Containerfile.responder-schema-migrator"
observer_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-observer
authority_capability_binary=/usr/local/bin/gaudere-local-goose-dialogue-responder-authority-capability

fail()
{
    printf 'gaudere responder authority schema3 production migration: FAIL: %s\n' "$*" >&2
    exit 1
}

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

running_image()
{
    "$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://'
}

network_mode()
{
    "$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null
}

provider_total()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';"
}

logical_dump_sha()
{
    sqlite3 -readonly "$1" '.dump' | sha256sum | awk '{print $1}'
}

table_dump_sha()
{
    sqlite3 -readonly "$1" ".dump $2" | sha256sum | awk '{print $1}'
}

responder_tables()
{
    sqlite3 -readonly "$1" "SELECT group_concat(name, '|') FROM (SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'local_goose_dialogue_responder_%' ORDER BY name);"
}

verify_secure_responder_file()
{
    [ -f "$responder_sidecar" ] && [ ! -L "$responder_sidecar" ] \
        || fail "responder sidecar is missing or unsafe"
    [ "$(stat -c '%a' "$responder_sidecar")" = "600" ] \
        || fail "responder sidecar mode is not 0600"
    [ "$(stat -c '%u' "$responder_sidecar")" = "$(id -u)" ] \
        || fail "responder sidecar is not owned by current user"
}

verify_common_state()
{
    [ "$(network_mode)" = "none" ] || fail "production network is not none"
    [ "$(provider_total)" = "$expected_provider_total" ] \
        || fail "provider total differs from expected value"
    [ "$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] \
        || fail "stimulus ledger is not empty"

    responder_cursor=$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")
    [ "$responder_cursor" = "3" ] || fail "production responder cursor is not exactly 3"
    sequence_four_kind=$(sqlite3 -readonly "$completion_sidecar" "SELECT speaker_kind FROM local_goose_dialogue_completion_event WHERE sequence=4;")
    [ "$sequence_four_kind" = "human" ] || fail "production completion sequence 4 is not human-authored"
    sequence_four_task=$(sqlite3 -readonly "$completion_sidecar" "SELECT task_id FROM local_goose_dialogue_completion_event WHERE sequence=4;")
    [ -n "$sequence_four_task" ] || fail "production completion sequence 4 task is missing"
    [ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=3 AND head_task_id='$sequence_four_task';")" = "1" ] \
        || fail "production sequence 4 is not the current preferred head"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND completion_sequence=3 AND kind=0 AND state=1;")" = "1" ] \
        || fail "production sequence 3 structural disposition differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND completion_sequence=4;")" = "0" ] \
        || fail "production sequence 4 already has responder disposition"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_lease WHERE state=0;')" = "0" ] \
        || fail "production unexpectedly has active responder lease"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_intent WHERE state=0;')" = "0" ] \
        || fail "production unexpectedly has prepared responder intent"
}

verify_schema2_prestate()
{
    verify_secure_responder_file
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "2" ] \
        || fail "production responder schema is not exactly v2"
    [ "$(responder_tables "$responder_sidecar")" = "local_goose_dialogue_responder_disposition|local_goose_dialogue_responder_intent|local_goose_dialogue_responder_lease" ] \
        || fail "production responder v2 table layout differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='local_goose_dialogue_responder_grant';")" = "0" ] \
        || fail "production unexpectedly contains responder grant table"
    [ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='local_goose_dialogue_responder_decision';")" = "0" ] \
        || fail "production unexpectedly contains responder decision table"
}

verify_schema3_state()
{
    verify_secure_responder_file
    [ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "3" ] \
        || fail "production responder schema is not exactly v3"
    [ "$(responder_tables "$responder_sidecar")" = "local_goose_dialogue_responder_decision|local_goose_dialogue_responder_disposition|local_goose_dialogue_responder_grant|local_goose_dialogue_responder_intent|local_goose_dialogue_responder_lease" ] \
        || fail "production responder v3 table layout differs"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_grant;')" = "0" ] \
        || fail "schema migration synthesized responder grants"
    [ "$(sqlite3 -readonly "$responder_sidecar" 'SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;')" = "0" ] \
        || fail "schema migration synthesized responder decisions"
}

run_migrator()
{
    directory=$1
    "$podman_command" run --rm --network none \
        --userns keep-id --user "$host_uid:$host_gid" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --security-opt label=disable \
        --cap-drop all \
        -v "$directory:/state:rw" \
        "$migrator_image" migrate \
        --responder-sidecar /state/local-goose-dialogue-responder.db \
        --authorize-v2-to-v3
}

run_observer_status_on_copy()
{
    directory=$1
    "$podman_command" run --rm --network none \
        --userns keep-id --user "$host_uid:$host_gid" \
        --read-only --read-only-tmpfs \
        --security-opt no-new-privileges --security-opt label=disable \
        --cap-drop all \
        -v "$directory:/state:rw" \
        --entrypoint "$observer_binary" \
        "sha256:$expected_image" status \
        --thread-sidecar /state/local-goose-dialogue-thread.db \
        --completion-sidecar /state/local-goose-dialogue-completion.db \
        --responder-sidecar /state/local-goose-dialogue-responder.db
}

[ "$authorization" = "AUTHORIZED_DIALOGUE_RESPONDER_AUTHORITY_SCHEMA3_MIGRATION" ] \
    || fail "explicit authority schema3 migration authorization token is required"

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 mktemp mkdir mv tar sha256sum awk sed stat id sleep grep; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done
for file in "$backup_script" "$migrator_containerfile" "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required file is missing or unsafe: $file"
done
[ "$("$podman_command" info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = "true" ] \
    || fail "production migration requires rootless Podman"
[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] \
    || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] \
    || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] \
    || fail "gaudere-agent checkout must be clean"

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
host_uid=$(id -u)
host_gid=$(id -g)
case "$agent_ref" in *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal Git SHA" ;; esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"

printf '=== VERIFY AUTHORITY SCHEMA3 MIGRATION PRESTATE ===\n'
[ "$(service_state)" = "active" ] || fail "production service must be active before migration"
[ "$(running_image)" = "$expected_image" ] || fail "production image differs from expected dormant-authority image"
verify_schema2_prestate
verify_common_state

state_sha_before=$(logical_dump_sha "$state_database")
cycle_sha_before=$(logical_dump_sha "$cycle_sidecar")
stimulus_sha_before=$(logical_dump_sha "$stimulus_sidecar")
thread_sha_before=$(logical_dump_sha "$thread_sidecar")
completion_sha_before=$(logical_dump_sha "$completion_sidecar")
lease_sha_before=$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_lease)
intent_sha_before=$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_intent)
disposition_sha_before=$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_disposition)

printf '=== BUILD CONFINED RESPONDER SCHEMA MIGRATOR ===\n'
migrator_image="localhost/gaudere-agent:responder-schema-migrator-$agent_ref"
"$podman_command" build \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --tag "$migrator_image" \
    --file "$migrator_containerfile" \
    "$repository_root"
[ "$("$podman_command" image inspect "$migrator_image" --format '{{index .Config.Labels "io.gaudere.agent.revision"}}')" = "$agent_ref" ] \
    || fail "migrator image provenance label differs"
help_output=$("$podman_command" run --rm --network none --read-only --read-only-tmpfs \
    --security-opt no-new-privileges --cap-drop all "$migrator_image" --help) \
    || fail "migrator help probe failed"
printf '%s\n' "$help_output"
printf '%s\n' "$help_output" | grep -Fq 'migrate --responder-sidecar PATH --authorize-v1-to-v2' \
    || fail "migrator lost v1-to-v2 compatibility surface"
printf '%s\n' "$help_output" | grep -Fq 'migrate --responder-sidecar PATH --authorize-v2-to-v3' \
    || fail "migrator v2-to-v3 surface is missing"

transition_root="$data_home/gaudere/.dialogue-responder-authority-schema3-migration"
mkdir -p -m 0700 "$transition_root"
workspace=$(mktemp -d "$transition_root/transition.XXXXXX")
rehearsal="$workspace/rehearsal"
mkdir -p -m 0700 "$rehearsal"

python3 - "$rehearsal" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" <<'PY'
import os
from pathlib import Path
import sqlite3
import sys

destination = Path(sys.argv[1])
for raw in sys.argv[2:]:
    source = Path(raw)
    target = destination / source.name
    src = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    try:
        dst = sqlite3.connect(target)
        try:
            src.backup(dst)
        finally:
            dst.close()
    finally:
        src.close()
    os.chmod(target, 0o600)
PY

printf '=== REHEARSE V2 -> V3 MIGRATION ON COPY ===\n'
rehearsal_output=$(run_migrator "$rehearsal") || fail "copy rehearsal migration failed"
printf '%s\n' "$rehearsal_output"
printf '%s\n' "$rehearsal_output" | grep -Fq 'RESPONDER_SCHEMA_BEFORE=2' || fail "rehearsal did not start from schema2"
printf '%s\n' "$rehearsal_output" | grep -Fq 'RESPONDER_SCHEMA_AFTER=3' || fail "rehearsal did not reach schema3"
printf '%s\n' "$rehearsal_output" | grep -Fq 'RESPONDER_AUTHORITY_GRANTS=0' || fail "rehearsal synthesized grants"
printf '%s\n' "$rehearsal_output" | grep -Fq 'RESPONDER_AUTHORITY_DECISIONS=0' || fail "rehearsal synthesized decisions"
rehearsal_responder="$rehearsal/local-goose-dialogue-responder.db"
[ "$(responder_tables "$rehearsal_responder")" = "local_goose_dialogue_responder_decision|local_goose_dialogue_responder_disposition|local_goose_dialogue_responder_grant|local_goose_dialogue_responder_intent|local_goose_dialogue_responder_lease" ] \
    || fail "rehearsal responder v3 table layout differs"
[ "$(table_dump_sha "$rehearsal_responder" local_goose_dialogue_responder_lease)" = "$lease_sha_before" ] \
    || fail "rehearsal migration changed historical leases"
[ "$(table_dump_sha "$rehearsal_responder" local_goose_dialogue_responder_intent)" = "$intent_sha_before" ] \
    || fail "rehearsal migration changed historical intents"
[ "$(table_dump_sha "$rehearsal_responder" local_goose_dialogue_responder_disposition)" = "$disposition_sha_before" ] \
    || fail "rehearsal migration changed historical dispositions"

rehearsal_status=$(run_observer_status_on_copy "$rehearsal") || fail "deployed image cannot reopen rehearsed schema3 copy"
printf '%s\n' "$rehearsal_status"
printf '%s\n' "$rehearsal_status" | grep -q '^classification=current_human$' || fail "rehearsal next event is not current_human"
printf '%s\n' "$rehearsal_status" | grep -q '^observer_armed=no$' || fail "rehearsal observer unexpectedly armed"
printf '%s\n' "$rehearsal_status" | grep -q '^consumer_last_sequence=3$' || fail "rehearsal responder cursor differs"
printf '%s\n' "$rehearsal_status" | grep -q '^next_sequence=4$' || fail "rehearsal sequence4 is not pending"

verify_schema2_prestate
verify_common_state
[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "rehearsal changed production state database"
[ "$(logical_dump_sha "$cycle_sidecar")" = "$cycle_sha_before" ] || fail "rehearsal changed production cycle state"
[ "$(logical_dump_sha "$stimulus_sidecar")" = "$stimulus_sha_before" ] || fail "rehearsal changed production stimulus state"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "rehearsal changed production thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "rehearsal changed production completion state"

backup_archive=""
state_mutated=0
service_stopped=0
committed=0
recover()
{
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = "1" ]; then exit "$status"; fi
    if [ "$state_mutated" = "0" ]; then
        if [ "$service_stopped" = "1" ]; then
            "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
        fi
        exit "$status"
    fi
    printf 'gaudere responder authority schema3 migration: recovery starting\n' >&2
    "$systemctl_command" --user stop "$service_name" >/dev/null 2>&1 || true
    failed_state="$workspace/failed-state"
    if [ ! -e "$failed_state" ]; then
        mv "$state_directory" "$failed_state" || true
        mkdir -p -m 0700 "$state_directory" || true
        tar -xzf "$backup_archive" -C "$state_directory" || true
    fi
    "$systemctl_command" --user start "$service_name" >/dev/null 2>&1 || true
    printf 'recovery_service=%s\n' "$(service_state)" >&2
    printf 'recovery_image=%s\n' "$(running_image)" >&2
    printf 'recovery_provider_total=%s\n' "$(provider_total 2>/dev/null || true)" >&2
    printf 'recovery_workspace=%s\n' "$workspace" >&2
    exit "$status"
}
trap recover EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '=== STOP / BACKUP PRODUCTION BEFORE SCHEMA MIGRATION ===\n'
"$systemctl_command" --user stop "$service_name"
service_stopped=1
[ "$(service_state)" = "inactive" ] || fail "production service did not stop cleanly"
verify_schema2_prestate
[ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total changed before stopped-state backup"
backup_archive=$(GAUDERE_STATE_DIR="$state_directory" sh "$backup_script")
[ -n "$backup_archive" ] && [ -f "$backup_archive" ] || fail "stopped-state backup was not created"
printf 'BACKUP=%s\n' "$backup_archive"

printf '=== MIGRATE ONLY RESPONDER SIDECAR V2 -> V3 ===\n'
state_mutated=1
migration_output=$(run_migrator "$state_directory") || fail "production responder schema migration failed"
printf '%s\n' "$migration_output"
printf '%s\n' "$migration_output" | grep -Fq 'RESPONDER_SCHEMA_MIGRATION=v2-to-v3' || fail "production migration marker differs"
verify_schema3_state
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_lease)" = "$lease_sha_before" ] \
    || fail "production migration changed historical leases"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_intent)" = "$intent_sha_before" ] \
    || fail "production migration changed historical intents"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_disposition)" = "$disposition_sha_before" ] \
    || fail "production migration changed historical dispositions"
[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "production migration changed state database"
[ "$(logical_dump_sha "$cycle_sidecar")" = "$cycle_sha_before" ] || fail "production migration changed cycle state"
[ "$(logical_dump_sha "$stimulus_sidecar")" = "$stimulus_sha_before" ] || fail "production migration changed stimulus state"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "production migration changed thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "production migration changed completion state"

printf '=== RESTART SAME DORMANT-AUTHORITY IMAGE ===\n'
"$systemctl_command" --user start "$service_name"
service_stopped=0
[ "$(service_state)" = "active" ] || fail "production service did not become active"
sleep 3
[ "$(service_state)" = "active" ] || fail "production service did not remain active"
[ "$(running_image)" = "$expected_image" ] || fail "production image changed during schema migration"
verify_schema3_state
verify_common_state
[ "$(logical_dump_sha "$state_database")" = "$state_sha_before" ] || fail "service restart changed state database"
[ "$(logical_dump_sha "$cycle_sidecar")" = "$cycle_sha_before" ] || fail "service restart changed cycle state"
[ "$(logical_dump_sha "$stimulus_sidecar")" = "$stimulus_sha_before" ] || fail "service restart changed stimulus state"
[ "$(logical_dump_sha "$thread_sidecar")" = "$thread_sha_before" ] || fail "service restart changed thread state"
[ "$(logical_dump_sha "$completion_sidecar")" = "$completion_sha_before" ] || fail "service restart changed completion state"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_lease)" = "$lease_sha_before" ] || fail "service restart changed historical leases"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_intent)" = "$intent_sha_before" ] || fail "service restart changed historical intents"
[ "$(table_dump_sha "$responder_sidecar" local_goose_dialogue_responder_disposition)" = "$disposition_sha_before" ] || fail "service restart changed historical dispositions"

post_status=$("$podman_command" exec gaudere-agent "$observer_binary" status \
    --thread-sidecar /var/lib/gaudere/local-goose-dialogue-thread.db \
    --completion-sidecar /var/lib/gaudere/local-goose-dialogue-completion.db \
    --responder-sidecar /var/lib/gaudere/local-goose-dialogue-responder.db) \
    || fail "post-migration observer status failed"
printf '%s\n' "$post_status"
printf '%s\n' "$post_status" | grep -q '^classification=current_human$' || fail "post-migration next event is not current_human"
printf '%s\n' "$post_status" | grep -q '^observer_armed=no$' || fail "observer unexpectedly armed after migration"
printf '%s\n' "$post_status" | grep -q '^consumer_last_sequence=3$' || fail "post-migration responder cursor differs"
printf '%s\n' "$post_status" | grep -q '^next_sequence=4$' || fail "post-migration sequence4 is not pending"

capability_output=$("$podman_command" exec gaudere-agent "$authority_capability_binary" --check) \
    || fail "post-migration dormant authority capability probe failed"
printf '%s\n' "$capability_output"
printf '%s\n' "$capability_output" | grep -Fq 'AUTHORITY_DEFAULT=disabled' || fail "authority default changed"
printf '%s\n' "$capability_output" | grep -Fq 'STATE_ACCESS=absent' || fail "authority capability gained state access"
printf '%s\n' "$capability_output" | grep -Fq 'MUTATION_SURFACE=absent' || fail "authority capability gained mutation surface"

committed=1
trap - EXIT HUP INT TERM

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$expected_image"
printf 'NETWORK=none\n'
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'PRODUCTION_RESPONDER_SCHEMA=3\n'
printf 'PRODUCTION_AUTHORITY_GRANTS=0\n'
printf 'PRODUCTION_AUTHORITY_DECISIONS=0\n'
printf 'PRODUCTION_RESPONDER_CURSOR=3\n'
printf 'PRODUCTION_NEXT_PENDING_SEQUENCE=4\n'
printf 'PRODUCTION_NEXT_CLASSIFICATION=current_human\n'
printf 'PRODUCTION_ACTIVE_LEASES=0\n'
printf 'PRODUCTION_PREPARED_INTENTS=0\n'
printf 'AUTHORITY_MODE=disabled-unwired\n'
printf 'BACKUP=%s\n' "$backup_archive"
printf 'TRANSITION_WORKSPACE=%s\n' "$workspace"
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_SCHEMA3_MIGRATION=PASS\n'
