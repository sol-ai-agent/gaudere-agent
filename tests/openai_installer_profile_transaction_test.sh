#!/bin/sh
set -eu

installer=scripts/install-openai-user-service.sh
workspace=$(mktemp -d)
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

fakebin="$workspace/bin"
config_home="$workspace/config"
data_home="$workspace/data"
state="$data_home/gaudere/state"
target="$config_home/containers/systemd/gaudere-agent.container"
reload_log="$workspace/reload.log"
image_id=sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
mkdir -p "$fakebin" "$state" "$(dirname "$target")"

cat > "$fakebin/podman" <<'SH'
#!/bin/sh
set -eu
case "$1:$2" in
    image:exists) exit 0 ;;
    image:inspect)
        printf '%s\n' "${GAUDERE_FAKE_IMAGE_ID:?}"
        exit 0
        ;;
    secret:exists) exit 0 ;;
esac
if [ "$1" = "run" ]; then
    printf 'Usage: gaudere-control --socket PATH [echo ID TEXT | openai ID TEXT | reflect ID OBJECTIVE | task ID | budget]\n' >&2
    exit 2
fi
exit 99
SH
chmod +x "$fakebin/podman"

cat > "$fakebin/systemctl" <<'SH'
#!/bin/sh
set -eu
if [ "$1" = "--user" ] && [ "$2" = "is-active" ]; then
    printf 'inactive\n'
    exit 3
fi
if [ "$1" = "--user" ] && [ "$2" = "daemon-reload" ]; then
    printf 'reload\n' >> "${GAUDERE_FAKE_RELOAD_LOG:?}"
    if [ "${GAUDERE_FAKE_RELOAD_FAIL:-0}" = "1" ]; then
        exit 77
    fi
    exit 0
fi
exit 99
SH
chmod +x "$fakebin/systemctl"

python3 - "$state/state.db" <<'PY'
import sqlite3
import sys
with sqlite3.connect(sys.argv[1]) as db:
    db.executescript("""
    PRAGMA user_version=3;
    CREATE TABLE tasks (id TEXT, kind TEXT, status INTEGER);
    """)
PY

printf 'ORIGINAL PROFILE\n' > "$target"
chmod 600 "$target"
: > "$reload_log"

if GAUDERE_FAKE_RELOAD_FAIL=1 \
    GAUDERE_FAKE_RELOAD_LOG="$reload_log" \
    GAUDERE_FAKE_IMAGE_ID="$image_id" \
    PATH="$fakebin:$PATH" \
    XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
    sh "$installer" > "$workspace/fail.out" 2> "$workspace/fail.err"; then
    printf 'installer unexpectedly survived daemon-reload failure\n' >&2
    exit 1
fi

grep -qx 'ORIGINAL PROFILE' "$target"
grep -q 'uncommitted profile replacement rolled back' "$workspace/fail.err"
[ "$(grep -c '^reload$' "$reload_log")" -ge 1 ]

: > "$reload_log"
GAUDERE_FAKE_RELOAD_LOG="$reload_log" \
GAUDERE_FAKE_IMAGE_ID="$image_id" \
PATH="$fakebin:$PATH" \
XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
sh "$installer" > "$workspace/pass.out"

grep -qx "Image=$image_id" "$target"
grep -q "runtime_image_id=$image_id" "$workspace/pass.out"
[ "$(grep -c '^reload$' "$reload_log")" -eq 1 ]

: > "$reload_log"
rm -f -- "$target"
GAUDERE_QUADLET_AUTOSTART=disarmed \
GAUDERE_FAKE_RELOAD_LOG="$reload_log" \
GAUDERE_FAKE_IMAGE_ID="$image_id" \
PATH="$fakebin:$PATH" \
XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
sh "$installer" > "$workspace/disarmed.out"

grep -qx "Image=$image_id" "$target"
if grep -q '^\[Install\]$\|^WantedBy=' "$target"; then
    printf 'disarmed installer retained an automatic-start directive\n' >&2
    exit 1
fi
grep -q '^gaudere OpenAI service install: quadlet_autostart=disarmed$' \
    "$workspace/disarmed.out"
[ "$(grep -c '^reload$' "$reload_log")" -eq 1 ]

printf 'gaudere OpenAI installer profile transaction test: PASS\n'
