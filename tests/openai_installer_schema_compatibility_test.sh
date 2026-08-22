#!/bin/sh
set -eu

installer=scripts/install-openai-user-service.sh
profile=deploy/quadlet/gaudere-agent-openai.container.in
workspace=$(mktemp -d)
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

fakebin="$workspace/bin"
config_home="$workspace/config"
state_directory="$workspace/state"
temporary_root="$workspace/tmp"
podman_log="$workspace/podman.log"
target_profile="$config_home/containers/systemd/gaudere-agent.container"
expected_profile="$workspace/profile.expected"
expected_image_id=sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
mkdir -p "$fakebin" "$state_directory" "$temporary_root"

python3 - "$profile" "$expected_profile" "$expected_image_id" <<'PY'
import pathlib
import sys
source, target = map(pathlib.Path, sys.argv[1:3])
image = sys.argv[3]
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
indexes = [i for i, line in enumerate(lines) if line.startswith("Image=")]
assert len(indexes) == 1
index = indexes[0]
ending = "\n" if lines[index].endswith("\n") else ""
lines[index] = f"Image={image}{ending}"
target.write_text("".join(lines), encoding="utf-8")
PY

cat > "$fakebin/podman" <<'SH'
#!/bin/sh
set -eu

log=${GAUDERE_FAKE_PODMAN_LOG:?}

if [ "$1" = "image" ] && [ "$2" = "exists" ]; then
    exit 0
fi
if [ "$1" = "image" ] && [ "$2" = "inspect" ]; then
    printf '%s\n' "${GAUDERE_FAKE_IMAGE_ID:?}"
    exit 0
fi
if [ "$1" = "secret" ] && [ "$2" = "exists" ]; then
    exit 0
fi
if [ "$1" != "run" ]; then
    exit 99
fi
shift

entrypoint=
network=
volume=
state_path=
check=false
wake=false
provider=false

while [ "$#" -gt 0 ]; do
    argument=$1
    printf 'arg=%s\n' "$argument" >> "$log"
    case "$argument" in
        --entrypoint)
            shift
            entrypoint=$1
            printf 'arg=%s\n' "$entrypoint" >> "$log"
            ;;
        --network)
            shift
            network=$1
            printf 'arg=%s\n' "$network" >> "$log"
            ;;
        --volume)
            shift
            volume=$1
            printf 'arg=%s\n' "$volume" >> "$log"
            ;;
        --state)
            shift
            state_path=$1
            printf 'arg=%s\n' "$state_path" >> "$log"
            ;;
        --check)
            check=true
            ;;
        --wake-intents)
            wake=true
            ;;
        --openai-model|--openai-secret|--secret-dir)
            provider=true
            ;;
    esac
    shift
done

if [ "$entrypoint" = "/usr/local/bin/gaudere-control" ]; then
    printf 'CONTROL\n' >> "$log"
    printf 'Usage: gaudere-control --socket PATH [echo ID TEXT | openai ID TEXT | reflect ID OBJECTIVE | task ID | budget]\n' >&2
    exit 2
fi

printf 'AGENT\n' >> "$log"
[ "$network" = "none" ] || {
    printf 'schema-v4 probe did not disable networking\n' >&2
    exit 40
}
[ "$wake" = "false" ] || {
    printf 'schema-v4 probe unexpectedly passed --wake-intents\n' >&2
    exit 41
}
[ "$provider" = "false" ] || {
    printf 'schema-v4 probe unexpectedly configured a provider or secret\n' >&2
    exit 42
}
[ "$check" = "true" ] && [ "$state_path" = "/var/lib/gaudere/state.db" ] || {
    printf 'schema-v4 probe did not use the default check path\n' >&2
    exit 43
}
[ -n "$volume" ] || {
    printf 'schema-v4 probe did not mount a disposable state copy\n' >&2
    exit 44
}

host_directory=${volume%%:*}
[ "$host_directory" != "$GAUDERE_STATE_DIR" ] || {
    printf 'schema-v4 probe mounted the production state directory\n' >&2
    exit 45
}

python3 - "$host_directory/state.db" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as db:
    assert db.execute("PRAGMA user_version").fetchone()[0] == 4
    assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
PY

if [ "${GAUDERE_FAKE_V4_INCOMPATIBLE:-0}" = "1" ]; then
    printf 'synthetic image rejects schema v4\n' >&2
    exit 46
fi

if [ "${GAUDERE_FAKE_V4_WAKE_LOG:-0}" = "1" ]; then
    printf 'gaudere-agent: explicit wake enabled scope=cognition.reflect.wake.v0 max_total=1 automatic_successor=false\n'
fi
printf 'gaudere-agent: running\n'
printf 'gaudere-agent: safe\n'
SH
chmod +x "$fakebin/podman"

cat > "$fakebin/systemctl" <<'SH'
#!/bin/sh
if [ "$1" = "--user" ] && [ "$2" = "is-active" ]; then
    printf 'inactive\n'
    exit 3
fi
if [ "$1" = "--user" ] && [ "$2" = "daemon-reload" ]; then
    exit 0
fi
exit 99
SH
chmod +x "$fakebin/systemctl"

write_schema()
{
    version=$1
    python3 - "$state_directory/state.db" "$version" <<'PY'
import pathlib
import sqlite3
import sys

database = pathlib.Path(sys.argv[1])
database.unlink(missing_ok=True)
with sqlite3.connect(database) as db:
    db.executescript(f"""
        PRAGMA user_version={int(sys.argv[2])};
        CREATE TABLE tasks (id TEXT, kind TEXT, status INTEGER);
    """)
PY
}

run_installer()
{
    PATH="$fakebin:$PATH" \
    TMPDIR="$temporary_root" \
    XDG_CONFIG_HOME="$config_home" \
    GAUDERE_STATE_DIR="$state_directory" \
    GAUDERE_FAKE_PODMAN_LOG="$podman_log" \
    GAUDERE_FAKE_IMAGE_ID="$expected_image_id" \
    sh "$installer"
}

remove_target_profile()
{
    python3 - "$target_profile" <<'PY'
import pathlib
import sys
pathlib.Path(sys.argv[1]).unlink(missing_ok=True)
PY
}

for version in 3 4; do
    write_schema "$version"
    remove_target_profile
    : > "$podman_log"
    run_installer > "$workspace/schema-$version.out"

    cmp "$expected_profile" "$target_profile"
    grep -qx "Image=$expected_image_id" "$target_profile"
    grep -q "runtime_image_id=$expected_image_id" "$workspace/schema-$version.out"
    grep -q 'no provider task was submitted and the service remains stopped' \
        "$workspace/schema-$version.out"
    if grep -q -- '--wake-intents' "$target_profile"; then
        printf 'schema %s install unexpectedly changed the profile to enable WakeIntent\n' \
            "$version" >&2
        exit 1
    fi

    if [ "$version" = "3" ]; then
        if grep -qx 'AGENT' "$podman_log"; then
            printf 'schema-v3 install unexpectedly ran the schema-v4 reopen probe\n' >&2
            exit 1
        fi
    else
        grep -qx 'AGENT' "$podman_log"
        grep -q 'schema v4 default reopen probe passed without WakeIntent' \
            "$workspace/schema-$version.out"
        if grep -qx 'arg=--wake-intents' "$podman_log"; then
            printf 'schema-v4 reopen probe passed --wake-intents\n' >&2
            exit 1
        fi
        test -z "$(find "$temporary_root" -mindepth 1 -maxdepth 1 -print -quit)"
    fi
done

for version in 0 2 5 42; do
    write_schema "$version"
    install -d -m 0700 "$(dirname "$target_profile")"
    printf 'preserve-existing-profile\n' > "$target_profile"
    : > "$podman_log"

    if run_installer > "$workspace/reject-$version.out" \
        2> "$workspace/reject-$version.err"; then
        printf 'installer unexpectedly accepted schema %s\n' "$version" >&2
        exit 1
    fi
    grep -q "requires production schema v3 or v4 (found $version)" \
        "$workspace/reject-$version.err"
    grep -qx 'preserve-existing-profile' "$target_profile"
    test ! -s "$podman_log"
done

write_schema 4
install -d -m 0700 "$(dirname "$target_profile")"
printf 'preserve-existing-profile\n' > "$target_profile"
: > "$podman_log"
if GAUDERE_FAKE_V4_INCOMPATIBLE=1 run_installer \
    > "$workspace/incompatible.out" 2> "$workspace/incompatible.err"; then
    printf 'installer unexpectedly accepted an image that rejects schema v4\n' >&2
    exit 1
fi
grep -q 'runtime image cannot safely reopen schema v4 without --wake-intents' \
    "$workspace/incompatible.err"
grep -qx 'preserve-existing-profile' "$target_profile"
grep -qx 'AGENT' "$podman_log"
test -z "$(find "$temporary_root" -mindepth 1 -maxdepth 1 -print -quit)"

write_schema 4
printf 'preserve-existing-profile\n' > "$target_profile"
: > "$podman_log"
if GAUDERE_FAKE_V4_WAKE_LOG=1 run_installer \
    > "$workspace/wake-log.out" 2> "$workspace/wake-log.err"; then
    printf 'installer unexpectedly accepted a default schema-v4 reopen that enabled WakeIntent\n' >&2
    exit 1
fi
grep -q 'schema-v4 compatibility probe enabled WakeIntent' \
    "$workspace/wake-log.err"
grep -qx 'preserve-existing-profile' "$target_profile"
test -z "$(find "$temporary_root" -mindepth 1 -maxdepth 1 -print -quit)"

printf 'gaudere OpenAI installer schema compatibility test: PASS\n'
