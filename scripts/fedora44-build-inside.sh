#!/bin/sh
set -eu

repo=/workspace/agent
prefix=${GAUDERE_LOCAL_PREFIX:-$repo/.gaudere-local/fedora44}
build_root=${GAUDERE_BUILD_ROOT:-$repo/.build-fedora44}
core_src=$build_root/gaudere-core
core_build=$build_root/core-build
agent_build=$build_root/agent-build

ref=$(tr -d '\r\n' < "$repo/gaudere.ref")
case "$ref" in
    *[!0-9a-f]*|'') echo "invalid gaudere.ref" >&2; exit 1 ;;
esac
[ "${#ref}" -eq 40 ] || { echo "gaudere.ref must be one 40-character SHA" >&2; exit 1; }

rm -rf "$core_src" "$core_build" "$agent_build" "$prefix"
mkdir -p "$build_root" "$prefix"

git clone --quiet https://github.com/sol-ai-agent/gaudere.git "$core_src"
git -C "$core_src" checkout --quiet --detach "$ref"
[ "$(git -C "$core_src" rev-parse HEAD)" = "$ref" ]

(
    cd "$core_src"
    autoreconf --install --force
)
mkdir -p "$core_build"
(
    cd "$core_build"
    "$core_src/configure" --prefix="$prefix" \
        CXXFLAGS="-O2 -Wall -Wextra -Wpedantic -Werror"
    make --jobs=2 check
    make install
)

(
    cd "$repo"
    autoreconf --install --force
)
mkdir -p "$agent_build"
(
    cd "$agent_build"
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
    export LD_LIBRARY_PATH="$prefix/lib"
    "$repo/configure" --prefix="$prefix" \
        CXXFLAGS="-O2 -Wall -Wextra -Wpedantic -Werror"
    make --jobs=2 check
    make install
)

printf 'fedora=44\n'
printf 'core_ref=%s\n' "$ref"
printf 'agent_ref=%s\n' "$(git -C "$repo" rev-parse HEAD)"
printf 'prefix=%s\n' "$prefix"
printf 'status=PASS\n'
