#!/bin/sh
set -eu

offline=deploy/quadlet/gaudere-agent.container
online=deploy/quadlet/gaudere-agent-openai.container.in
installer=scripts/install-openai-user-service.sh

fail()
{
    printf 'gaudere OpenAI service config validation: %s\n' "$*" >&2
    exit 1
}

for file in "$offline" "$online" "$installer"; do
    [ -f "$file" ] || fail "missing file: $file"
done

# Offline remains the default and must expose no provider capability.
grep -qx 'Network=none' "$offline" \
    || fail "offline Quadlet must retain Network=none"
if grep -q '^Secret=' "$offline"; then
    fail "offline Quadlet must not mount a provider secret"
fi
if grep -q -- '--openai-model' "$offline"; then
    fail "offline Quadlet must not enable an OpenAI model"
fi
if grep -q '^PublishPort=' "$offline"; then
    fail "offline Quadlet must not publish a port"
fi

# Online is an explicit capability profile for the same single service/state owner.
grep -qx 'ContainerName=gaudere-agent' "$online" \
    || fail "OpenAI profile must retain the same container identity"
grep -q '^Exec=--state /var/lib/gaudere/state.db --control-socket /tmp/gaudere-control.sock --openai-model gpt-5.6-sol --openai-secret gaudere-openai-api-key$' "$online" \
    || fail "OpenAI profile must use fixed state/socket/model/secret arguments"
grep -qx 'Secret=gaudere-openai-api-key,target=gaudere-openai-api-key,uid=1000,gid=1000,mode=400' "$online" \
    || fail "OpenAI profile must mount only the expected restricted secret"
grep -qx 'Pull=never' "$online" \
    || fail "OpenAI profile must not pull a runtime image at service start"
if grep -q '^Network=none' "$online"; then
    fail "OpenAI profile cannot retain Network=none"
fi
if grep -q '^PublishPort=' "$online"; then
    fail "OpenAI profile must not expose an inbound host port"
fi
if grep -q -- '--openai-once' "$online"; then
    fail "service startup must never contain a one-shot provider task"
fi

# Hardening that must be identical in both profiles.
for setting in \
    'UserNS=keep-id' \
    'Volume=%h/.local/share/gaudere/state:/var/lib/gaudere:Z' \
    'ReadOnly=true' \
    'ReadOnlyTmpfs=true' \
    'NoNewPrivileges=true' \
    'DropCapability=all' \
    'PidsLimit=64' \
    'Memory=256m' \
    'StopSignal=SIGTERM' \
    'StopTimeout=30'; do
    grep -qx "$setting" "$offline" \
        || fail "offline profile missing hardening: $setting"
    grep -qx "$setting" "$online" \
        || fail "OpenAI profile missing hardening: $setting"
done

sh -n "$installer"

printf 'gaudere OpenAI service config validation: PASS\n'
