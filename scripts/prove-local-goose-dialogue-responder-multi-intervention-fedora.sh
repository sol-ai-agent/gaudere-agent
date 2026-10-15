#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
systemctl_command=${SYSTEMCTL:-systemctl}
service_name=${GAUDERE_SERVICE_NAME:-gaudere-agent.service}
expected_provider_total=${GAUDERE_EXPECTED_PROVIDER_TOTAL:-10}
expected_image=${GAUDERE_EXPECTED_PRODUCTION_IMAGE:-ef14db77fa3b3d21ff3fed9b37ead611ca8bf533cff424da810ac6ac4d1a77eb}
consumer_id=system-responder-v1
thread_alias=main

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
proof_root="$data_home/gaudere/.dialogue-responder-multi-intervention-isolated-proof"
workspace=""
proof_builder_image=""
proof_ok=0

fail()
{
    printf 'gaudere dialogue responder multi-intervention isolated proof: FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    rc=$?
    trap - EXIT HUP INT TERM
    if [ -n "$proof_builder_image" ]; then
        "$podman_command" image rm "$proof_builder_image" >/dev/null 2>&1 || true
    fi
    if [ "$proof_ok" = "1" ] && [ -n "$workspace" ] && [ -d "$workspace" ]; then
        rm -rf "$workspace"
    elif [ -n "$workspace" ] && [ -d "$workspace" ]; then
        printf 'gaudere dialogue responder multi-intervention isolated proof: diagnostic directory preserved: %s\n' "$workspace" >&2
    fi
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

for command in "$podman_command" "$systemctl_command" git python3 sqlite3 sha256sum awk sed grep mkdir mktemp rm stat id; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

[ "$("$podman_command" info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" = "true" ] || fail "isolated proof requires rootless Podman"
[ "$(git -C "$repository_root" rev-parse --show-toplevel)" = "$repository_root" ] || fail "script must belong to gaudere-agent checkout"
[ "$(git -C "$repository_root" branch --show-current)" = "main" ] || fail "gaudere-agent checkout must be on main"
[ -z "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ] || fail "gaudere-agent checkout must be clean"

for file in "$state_database" "$cycle_sidecar" "$stimulus_sidecar" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar"; do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "required production file is missing or unsafe: $file"
done

agent_ref=$(git -C "$repository_root" rev-parse HEAD)
core_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
case "$agent_ref" in
    *[!0-9a-f]*|'') fail "Agent HEAD is not one lowercase hexadecimal Git SHA" ;;
esac
[ "${#agent_ref}" -eq 40 ] || fail "Agent HEAD is not 40 characters"
case "$core_ref" in
    *[!0-9a-f]*|'') fail "gaudere.ref is not one lowercase hexadecimal Git SHA" ;;
esac
[ "${#core_ref}" -eq 40 ] || fail "gaudere.ref is not 40 characters"

service_state()
{
    "$systemctl_command" --user is-active "$service_name" 2>/dev/null || true
}

running_image()
{
    "$podman_command" inspect gaudere-agent --format '{{.Image}}' 2>/dev/null | sed 's/^sha256://'
}

provider_total()
{
    sqlite3 -readonly "$state_database" "SELECT COUNT(*) FROM budget_consumptions WHERE scope='provider.call:openai.responses';"
}

cycle_cursor()
{
    sqlite3 -readonly "$cycle_sidecar" "SELECT revision||'|'||generation||'|'||state||'|'||COALESCE(due_at_ms,'')||'|'||COALESCE(captured_at_ms,'')||'|'||COALESCE(current_task_id,'')||'|'||COALESCE(predecessor_task_id,'')||'|'||COALESCE(blocked_reason,'') FROM local_goose_cycle_cursor WHERE scope='cognition.local-goose-cycle.v1';"
}

production_signature()
{
    printf '%s\n' \
        "service=$(service_state)" \
        "image=$(running_image)" \
        "network=$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" \
        "state=$(sqlite3 -readonly "$state_database" '.dump' | sha256sum | awk '{print $1}')" \
        "cycle=$(sqlite3 -readonly "$cycle_sidecar" '.dump' | sha256sum | awk '{print $1}')" \
        "stimulus=$(sqlite3 -readonly "$stimulus_sidecar" '.dump' | sha256sum | awk '{print $1}')" \
        "thread=$(sqlite3 -readonly "$thread_sidecar" '.dump' | sha256sum | awk '{print $1}')" \
        "completion=$(sqlite3 -readonly "$completion_sidecar" '.dump' | sha256sum | awk '{print $1}')" \
        "responder=$(sqlite3 -readonly "$responder_sidecar" '.dump' | sha256sum | awk '{print $1}')"
}

printf '=== VERIFY POST-FIRST-ACTIVATION PRODUCTION CHECKPOINT ===\n'
[ "$(service_state)" = "active" ] || fail "production service is not active"
[ "$(running_image)" = "$expected_image" ] || fail "production image differs"
[ "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)" = "none" ] || fail "production network is not none"
[ "$(provider_total)" = "$expected_provider_total" ] || fail "provider total differs"
[ "$(sqlite3 -readonly "$stimulus_sidecar" 'SELECT COUNT(*) FROM local_goose_cycle_stimuli;')" = "0" ] || fail "stimulus ledger is not empty"
[ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "production responder schema is not v2"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] || fail "production responder cursor is not 3"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT speaker_kind FROM local_goose_dialogue_completion_event WHERE sequence=4;")" = "human" ] || fail "production sequence 4 is not human-authored"
seq4_task=$(sqlite3 -readonly "$completion_sidecar" "SELECT task_id FROM local_goose_dialogue_completion_event WHERE sequence=4;")
[ -n "$seq4_task" ] || fail "production sequence 4 task is missing"
[ "$(sqlite3 -readonly "$thread_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_thread_head WHERE alias='$thread_alias' AND revision=3 AND head_task_id='$seq4_task';")" = "1" ] || fail "production current human preferred head differs"
[ "$(sqlite3 -readonly "$responder_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition WHERE consumer_id='$consumer_id' AND completion_sequence=3 AND kind=0 AND state=1;")" = "1" ] || fail "production sequence 3 durable disposition differs"

production_before=$(production_signature)
provider_before=$(provider_total)
cycle_before=$(cycle_cursor)

mkdir -p -m 0700 "$proof_root"
workspace=$(mktemp -d "$proof_root/run.XXXXXX")
scenario="$workspace/scenario"
mkdir -p -m 0700 "$scenario"

python3 - "$scenario" "$state_database" "$thread_sidecar" "$completion_sidecar" "$responder_sidecar" <<'PY'
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

[ "$(sqlite3 -readonly "$scenario/local-goose-dialogue-responder.db" 'PRAGMA user_version;')" = "2" ] || fail "copied responder schema differs"
[ "$(sqlite3 -readonly "$scenario/local-goose-dialogue-completion.db" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] || fail "copied responder cursor differs"

printf '=== BUILD MULTI-INTERVENTION PROOF HELPER IN ROOTLESS PODMAN ===\n'
proof_builder_image="localhost/gaudere-agent:multi-intervention-proof-builder-$agent_ref"
"$podman_command" build \
    --target builder \
    --build-arg "BUILDER_IMAGE=registry.fedoraproject.org/fedora:44" \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --build-arg "GAUDERE_REF=$core_ref" \
    --tag "$proof_builder_image" \
    --file "$repository_root/Containerfile" \
    "$repository_root"

"$podman_command" run --rm --network none \
    --user 0:0 \
    --security-opt no-new-privileges --cap-drop all \
    -e PKG_CONFIG_PATH=/opt/gaudere/lib/pkgconfig \
    -e LD_LIBRARY_PATH=/opt/gaudere/lib \
    -v "$workspace:/proof:Z" \
    "$proof_builder_image" \
    sh -c '
        set -eu
        cflags=$(pkg-config --cflags gaudere-persistence-sqlite nlohmann_json)
        libs=$(pkg-config --libs gaudere-persistence-sqlite nlohmann_json)
        g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
            $cflags -I/src/gaudere-agent/src \
            /src/gaudere-agent/src/local_goose_dialogue_responder_multi_intervention_proof_main.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponderObserver.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponderDispatcher.cpp \
            /src/gaudere-agent/src/LocalGooseDialoguePreferredSubmitter.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponderStore.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueResponder.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueCompletionFeed.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueThreadStore.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueV2.cpp \
            /src/gaudere-agent/src/LocalGooseDialogueV3.cpp \
            /src/gaudere-agent/src/Sha256.cpp \
            -o /proof/multi-intervention-proof \
            $libs -lsqlite3
        /proof/multi-intervention-proof --help | grep -Fq -- "--state PATH --thread-sidecar PATH"
    '

printf '=== RUN TWO-INTERVENTION / NO-SELF-CHAIN PROOF ON COPIED STATE ===\n'
proof_output=$("$podman_command" run --rm --network none \
    --user 0:0 \
    --read-only --read-only-tmpfs \
    --security-opt no-new-privileges --cap-drop all \
    -e LD_LIBRARY_PATH=/opt/gaudere/lib \
    -v "$scenario:/state:rw,Z" \
    -v "$workspace:/proof:ro,Z" \
    "$proof_builder_image" \
    /proof/multi-intervention-proof \
    --state /state/state.db \
    --thread-sidecar /state/local-goose-dialogue-thread.db \
    --completion-sidecar /state/local-goose-dialogue-completion.db \
    --responder-sidecar /state/local-goose-dialogue-responder.db) \
    || fail "multi-intervention helper failed"
printf '%s\n' "$proof_output"
printf '%s\n' "$proof_output" | grep -Fq 'GAUDERE_DIALOGUE_RESPONDER_MULTI_INTERVENTION_PROOF=PASS' || fail "multi-intervention helper PASS marker missing"
printf '%s\n' "$proof_output" | grep -Fq 'LEASE_TURNS_COMMITTED=2' || fail "finite lease turn count differs"
printf '%s\n' "$proof_output" | grep -Fq 'LEASE_STATE=exhausted' || fail "finite lease did not exhaust"
printf '%s\n' "$proof_output" | grep -Fq 'FINAL_RESPONDER_CURSOR=7' || fail "copied responder cursor differs"
printf '%s\n' "$proof_output" | grep -Fq 'FINAL_CLASSIFICATION=current_human' || fail "final copied event is not held human"
printf '%s\n' "$proof_output" | grep -Fq 'SELF_CHAIN=blocked' || fail "self-chain proof missing"
printf '%s\n' "$proof_output" | grep -Fq 'AUTO_RENEW=blocked' || fail "auto-renew proof missing"
printf '%s\n' "$proof_output" | grep -Fq 'MIN_INTERVAL=fail-closed' || fail "minimum interval proof missing"
printf '%s\n' "$proof_output" | grep -Fq 'CRASH_RETRY=idempotent' || fail "crash retry proof missing"

printf '=== VERIFY PRODUCTION WAS NOT TOUCHED ===\n'
production_after=$(production_signature)
[ "$production_after" = "$production_before" ] || fail "production signature changed during isolated proof"
[ "$(provider_total)" = "$provider_before" ] || fail "production provider total changed"
[ "$(cycle_cursor)" = "$cycle_before" ] || fail "production cycle cursor changed"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor WHERE consumer_id='$consumer_id';")" = "3" ] || fail "production responder cursor changed"
[ "$(sqlite3 -readonly "$responder_sidecar" 'PRAGMA user_version;')" = "2" ] || fail "production responder schema changed"
[ "$(sqlite3 -readonly "$completion_sidecar" "SELECT COUNT(*) FROM local_goose_dialogue_completion_event WHERE sequence=4 AND speaker_kind='human';")" = "1" ] || fail "production human sequence 4 changed"

printf 'AGENT_REF=%s\n' "$agent_ref"
printf 'CORE_REF=%s\n' "$core_ref"
printf 'PRODUCTION_IMAGE=%s\n' "$(running_image)"
printf 'NETWORK=%s\n' "$("$podman_command" inspect gaudere-agent --format '{{.HostConfig.NetworkMode}}' 2>/dev/null)"
printf 'PROVIDER_TOTAL=%s\n' "$(provider_total)"
printf 'PRODUCTION_RESPONDER_SCHEMA=2\n'
printf 'PRODUCTION_RESPONDER_CURSOR=3\n'
printf 'PRODUCTION_NEXT_PENDING_SEQUENCE=4\n'
printf 'PRODUCTION_NEXT_CLASSIFICATION=current_human\n'
printf 'PROOF_FINAL_RESPONDER_CURSOR=7\n'
printf 'PROOF_FINAL_PENDING_SEQUENCE=8\n'
printf 'PROOF_LEASE_TURNS=2\n'
printf 'PROOF_LEASE_STATE=exhausted\n'
printf 'PROOF_SELF_CHAIN=blocked\n'
printf 'PROOF_AUTO_RENEW=blocked\n'
printf 'PROOF_MIN_INTERVAL=fail-closed\n'
printf 'PROOF_CRASH_RETRY=idempotent\n'
proof_ok=1
printf 'FEDORA_GAUDERE_DIALOGUE_RESPONDER_MULTI_INTERVENTION_ISOLATED=PASS\n'
