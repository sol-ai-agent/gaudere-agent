#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
verifier="$repository_root/scripts/verify-image-provenance.sh"
capture="$repository_root/scripts/capture-schema-v4-image-rollback.sh"
validator="$repository_root/scripts/validate-schema-v4-image-provenance.sh"

workspace=$(mktemp -d "${TMPDIR:-/tmp}/gaudere-image-provenance-test.XXXXXX")
fakebin="$workspace/bin"
fake_state="$workspace/fake-state"
mkdir -p "$fakebin" "$fake_state"

cleanup()
{
    rm -rf "$workspace"
}
trap cleanup EXIT HUP INT TERM

candidate=localhost/gaudere-agent:candidate-v4-ci
rollback=localhost/gaudere-agent:rollback-before-v4-ci
current=localhost/gaudere-agent:dev
agent_ref=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
core_ref=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
wrong_ref=ffffffffffffffffffffffffffffffffffffffff
candidate_id=sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
rollback_id=sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
drift_id=sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee
candidate_observed=${candidate_id#sha256:}
rollback_observed=${rollback_id#sha256:}
drift_observed=${drift_id#sha256:}

printf '%s\n' "$candidate_observed" > "$fake_state/candidate.id"
printf '%s\n' "$rollback_observed" > "$fake_state/rollback.id"
printf '%s\n' "$candidate_observed" > "$fake_state/current.id"
printf '%s\n' "$agent_ref" > "$fake_state/agent.ref"
printf '%s\n' "$core_ref" > "$fake_state/core.ref"

fake_podman="$fakebin/podman"
cat > "$fake_podman" <<'SH'
#!/bin/sh
set -eu

id_file()
{
    case "$1" in
        "$GAUDERE_FAKE_CANDIDATE") printf '%s\n' "$GAUDERE_FAKE_STATE/candidate.id" ;;
        "$GAUDERE_FAKE_ROLLBACK") printf '%s\n' "$GAUDERE_FAKE_STATE/rollback.id" ;;
        "$GAUDERE_FAKE_CURRENT") printf '%s\n' "$GAUDERE_FAKE_STATE/current.id" ;;
        *) exit 2 ;;
    esac
}

if [ "$1" = "image" ] && [ "$2" = "exists" ]; then
    file=$(id_file "$3") || exit 1
    [ -f "$file" ]
    exit
fi

if [ "$1" = "image" ] && [ "$2" = "inspect" ] && [ "$3" = "--format" ]; then
    format=$4
    reference=$5
    file=$(id_file "$reference") || exit 1
    [ -f "$file" ] || exit 1
    case "$format" in
        '{{.Id}}') cat "$file" ;;
        *'org.opencontainers.image.revision'*) cat "$GAUDERE_FAKE_STATE/agent.ref" ;;
        *'io.gaudere.agent.revision'*) cat "$GAUDERE_FAKE_STATE/agent.ref" ;;
        *'io.gaudere.core.revision'*) cat "$GAUDERE_FAKE_STATE/core.ref" ;;
        *) exit 3 ;;
    esac
    exit
fi

if [ "$1" = "tag" ]; then
    source=$2
    target=$3
    [ "$target" = "$GAUDERE_FAKE_ROLLBACK" ] || exit 4
    case "$source" in
        sha256:*) printf '%s\n' "$source" > "$GAUDERE_FAKE_STATE/rollback.id" ;;
        *)
            file=$(id_file "$source") || exit 1
            cat "$file" > "$GAUDERE_FAKE_STATE/rollback.id"
            ;;
    esac
    exit
fi

exit 99
SH
chmod +x "$fake_podman"

copy_validator="$workspace/copy-validator"
cat > "$copy_validator" <<'SH'
#!/bin/sh
set -eu
printf 'image=%s archive=%s task=%s\n' \
    "$GAUDERE_IMAGE" "$1" "${2:-}" >> "$GAUDERE_FAKE_COPY_LOG"
case "${GAUDERE_FAKE_DRIFT:-none}" in
    none) ;;
    candidate)
        printf '%s\n' "$GAUDERE_FAKE_DRIFT_ID" \
            > "$GAUDERE_FAKE_STATE/candidate.id"
        ;;
    rollback)
        printf '%s\n' "$GAUDERE_FAKE_DRIFT_ID" \
            > "$GAUDERE_FAKE_STATE/rollback.id"
        ;;
    *) exit 96 ;;
esac
printf 'gaudere schema v4 migration copy validation: PASS\n'
SH
chmod +x "$copy_validator"

archive="$workspace/backup.tar.gz"
: > "$archive"
copy_log="$workspace/copy.log"
: > "$copy_log"

export GAUDERE_FAKE_STATE="$fake_state"
export GAUDERE_FAKE_CANDIDATE="$candidate"
export GAUDERE_FAKE_ROLLBACK="$rollback"
export GAUDERE_FAKE_CURRENT="$current"
export GAUDERE_FAKE_COPY_LOG="$copy_log"
export GAUDERE_FAKE_DRIFT_ID="$drift_observed"

printf '\n==> exact candidate provenance succeeds\n'
PODMAN="$fake_podman" sh "$verifier" \
    "$candidate" "$agent_ref" "$core_ref" "$candidate_id" \
    > "$workspace/verifier.out"
grep -q '^image_provenance=PASS$' "$workspace/verifier.out"

printf '\n==> false Agent/Core provenance fails closed\n'
if PODMAN="$fake_podman" sh "$verifier" \
        "$candidate" "$wrong_ref" "$core_ref" "$candidate_id" \
        > "$workspace/wrong-agent.out" 2> "$workspace/wrong-agent.err"; then
    printf 'provenance verifier accepted a false Agent revision\n' >&2
    exit 1
fi
grep -q 'OCI Agent revision mismatch' "$workspace/wrong-agent.err"
if PODMAN="$fake_podman" sh "$verifier" \
        "$candidate" "$agent_ref" "$wrong_ref" "$candidate_id" \
        > "$workspace/wrong-core.out" 2> "$workspace/wrong-core.err"; then
    printf 'provenance verifier accepted a false Core revision\n' >&2
    exit 1
fi
grep -q 'Gaudere Core revision mismatch' "$workspace/wrong-core.err"

printf '\n==> rollback capture pins the old ID before candidate work\n'
rm -f "$fake_state/rollback.id"
manifest="$workspace/rollback.manifest"
PODMAN="$fake_podman" \
GAUDERE_CURRENT_IMAGE="$current" \
GAUDERE_ROLLBACK_IMAGE="$rollback" \
GAUDERE_ROLLBACK_MANIFEST="$manifest" \
    sh "$capture" > "$workspace/capture.out"
grep -q '^rollback_capture=PASS$' "$workspace/capture.out"
grep -q "^source_image_id=$candidate_id$" "$manifest"
grep -q "^rollback_image_id=$candidate_id$" "$manifest"
if PODMAN="$fake_podman" \
        GAUDERE_CURRENT_IMAGE="$current" \
        GAUDERE_ROLLBACK_IMAGE="$rollback" \
        GAUDERE_ROLLBACK_MANIFEST="$workspace/second.manifest" \
        sh "$capture" > "$workspace/recapture.out" 2> "$workspace/recapture.err"; then
    printf 'rollback capture overwrote an existing rollback tag\n' >&2
    exit 1
fi
grep -q 'rollback tag already exists' "$workspace/recapture.err"

run_validator()
{
    PODMAN="$fake_podman" \
    GAUDERE_TEST_MODE=1 \
    GAUDERE_SCHEMA_V4_COPY_VALIDATOR="$copy_validator" \
    GAUDERE_CANDIDATE_IMAGE="$candidate" \
    GAUDERE_EXPECTED_AGENT_REF="$agent_ref" \
    GAUDERE_EXPECTED_CORE_REF="$core_ref" \
    GAUDERE_EXPECTED_CANDIDATE_ID="$candidate_id" \
    GAUDERE_ROLLBACK_IMAGE="$rollback" \
    GAUDERE_EXPECTED_ROLLBACK_ID="$rollback_id" \
        sh "$validator" "$archive" history-task
}

printf '\n==> complete disposable proof preserves both image identities\n'
printf '%s\n' "$candidate_observed" > "$fake_state/candidate.id"
printf '%s\n' "$rollback_observed" > "$fake_state/rollback.id"
GAUDERE_FAKE_DRIFT=none run_validator > "$workspace/validator.out"
grep -q '^image_tag_drift=NONE$' "$workspace/validator.out"
grep -q '^provider_effects=0$' "$workspace/validator.out"
grep -q '^wake_effects=0$' "$workspace/validator.out"
grep -q '^gaudere schema v4 image provenance validation: PASS$' \
    "$workspace/validator.out"
grep -q "image=$candidate archive=$archive task=history-task" "$copy_log"

printf '\n==> candidate tag drift during proof fails closed\n'
printf '%s\n' "$candidate_observed" > "$fake_state/candidate.id"
printf '%s\n' "$rollback_observed" > "$fake_state/rollback.id"
if GAUDERE_FAKE_DRIFT=candidate run_validator \
        > "$workspace/candidate-drift.out" 2> "$workspace/candidate-drift.err"; then
    printf 'validator accepted candidate tag drift\n' >&2
    exit 1
fi
grep -q 'image ID mismatch' "$workspace/candidate-drift.err"

printf '\n==> rollback tag drift during proof fails closed\n'
printf '%s\n' "$candidate_observed" > "$fake_state/candidate.id"
printf '%s\n' "$rollback_observed" > "$fake_state/rollback.id"
if GAUDERE_FAKE_DRIFT=rollback run_validator \
        > "$workspace/rollback-drift.out" 2> "$workspace/rollback-drift.err"; then
    printf 'validator accepted rollback tag drift\n' >&2
    exit 1
fi
grep -q 'rollback image drifted' "$workspace/rollback-drift.err"

printf '\n==> pre-existing derived-tag drift fails before disposable work\n'
printf '%s\n' "$drift_observed" > "$fake_state/candidate.id"
printf '%s\n' "$rollback_observed" > "$fake_state/rollback.id"
lines_before=$(wc -l < "$copy_log")
if GAUDERE_FAKE_DRIFT=none run_validator \
        > "$workspace/pre-drift.out" 2> "$workspace/pre-drift.err"; then
    printf 'validator accepted a candidate tag already pointing elsewhere\n' >&2
    exit 1
fi
grep -q 'image ID mismatch' "$workspace/pre-drift.err"
lines_after=$(wc -l < "$copy_log")
[ "$lines_before" = "$lines_after" ]

printf 'gaudere image provenance validator tests: PASS\n'
