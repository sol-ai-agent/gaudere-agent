#!/bin/sh
set -eu

podman_command=${PODMAN:-podman}
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
image_tag=${GAUDERE_IMAGE_TAG:-localhost/gaudere-agent:dev}
provenance_verifier="$script_directory/verify-image-provenance.sh"

if ! command -v "$podman_command" >/dev/null 2>&1; then
    echo "podman is required" >&2
    exit 1
fi
if ! command -v git >/dev/null 2>&1; then
    echo "git is required" >&2
    exit 1
fi
[ -x "$provenance_verifier" ] || {
    echo "image provenance verifier is required: $provenance_verifier" >&2
    exit 1
}
[ -n "$image_tag" ] || {
    echo "GAUDERE_IMAGE_TAG must not be empty" >&2
    exit 1
}
case "$image_tag" in
    *[!A-Za-z0-9._:/@-]*)
        echo "GAUDERE_IMAGE_TAG contains unsupported characters" >&2
        exit 1
        ;;
esac

git_root=$(git -C "$repository_root" rev-parse --show-toplevel 2>/dev/null) || {
    echo "gaudere-agent checkout is required" >&2
    exit 1
}
[ "$git_root" = "$repository_root" ] || {
    echo "build context must be the gaudere-agent checkout root" >&2
    exit 1
}
agent_ref=$(git -C "$repository_root" rev-parse HEAD)
case "$agent_ref" in
    *[!0-9a-f]*|'')
        echo "Agent HEAD must be one lowercase hexadecimal Git commit SHA" >&2
        exit 1
        ;;
esac
[ "${#agent_ref}" -eq 40 ] || {
    echo "Agent HEAD must be one 40-character Git commit SHA" >&2
    exit 1
}
if [ -n "$(git -C "$repository_root" status --porcelain --untracked-files=normal)" ]; then
    echo "gaudere-agent checkout must be clean before an attributable image build" >&2
    exit 1
fi

# Keep the normal host build aligned with CI and the Containerfile dependency
# names. Alternative builders remain possible, but only as an explicit operator
# choice; do not silently prefer an unrelated local image.
builder_image=${GAUDERE_BUILDER_IMAGE:-registry.fedoraproject.org/fedora:44}

if [ ! -f "$repository_root/gaudere.ref" ]; then
    echo "gaudere.ref is required" >&2
    exit 1
fi
gaudere_ref=$(tr -d '\r\n' < "$repository_root/gaudere.ref")
case "$gaudere_ref" in
    *[!0-9a-f]*|'')
        echo "gaudere.ref must contain one lowercase hexadecimal Git commit SHA" >&2
        exit 1
        ;;
esac
if [ "${#gaudere_ref}" -ne 40 ]; then
    echo "gaudere.ref must contain one 40-character Git commit SHA" >&2
    exit 1
fi

echo "Using builder image: $builder_image"
echo "Using Gaudere Agent ref: $agent_ref"
echo "Using Gaudere ref: $gaudere_ref"
echo "Using image tag: $image_tag"

"$podman_command" build \
    --build-arg "BUILDER_IMAGE=$builder_image" \
    --build-arg "GAUDERE_AGENT_REF=$agent_ref" \
    --build-arg "GAUDERE_REF=$gaudere_ref" \
    --tag "$image_tag" \
    --file "$repository_root/Containerfile" \
    "$repository_root"

PODMAN="$podman_command" sh "$provenance_verifier" \
    "$image_tag" "$agent_ref" "$gaudere_ref"
