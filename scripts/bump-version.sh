#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 MAJOR.MINOR.PATCH[-PRERELEASE]" >&2
    exit 2
fi

if ! printf '%s\n' "$1" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z][0-9A-Za-z.-]*)?$'; then
    echo "Version must be SemVer such as 0.9.1 or 1.0.0-RC1." >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_dir=$(dirname -- "$script_dir")
version=$1

rewrite_file() {
    rewrite_source=$1
    rewrite_expression=$2
    rewrite_temp=$(mktemp "${TMPDIR:-/tmp}/pvt-version.XXXXXX")
    if ! sed -E "$rewrite_expression" "$rewrite_source" > "$rewrite_temp"; then
        rm -f "$rewrite_temp"
        return 1
    fi
    mv "$rewrite_temp" "$rewrite_source"
}

printf '%s\n' "$version" > "$repository_dir/VERSION"
rewrite_file "$repository_dir/README.md" \
    "s/(Current product version: \\*\\*)[^*]+(\\*\\*)/\\1${version}\\2/"
rewrite_file "$repository_dir/debian/pvt-render.1" \
    '1s/(Procedural Visualizer Tool )[^\"]+(\" \"User Commands)/\1'"${version}"'\2/'
rewrite_file "$repository_dir/debian/procedural-visualizer-tool.1" \
    '1s/(Procedural Visualizer Tool )[^\"]+(\" \"User Commands)/\1'"${version}"'\2/'

debian_version=$(printf '%s\n' "$version" | sed 's/-/~/')
if ! head -n 1 "$repository_dir/debian/changelog" |
        grep -Fq "procedural-visualizer-tool ($debian_version)"; then
    maintainer_name=${DEBFULLNAME:-$(git -C "$repository_dir" config user.name || true)}
    maintainer_email=${DEBEMAIL:-$(git -C "$repository_dir" config user.email || true)}
    : "${maintainer_name:=PVT Maintainers}"
    : "${maintainer_email:=noreply@example.com}"
    changelog_temp=$(mktemp "${TMPDIR:-/tmp}/pvt-changelog.XXXXXX")
    {
        printf 'procedural-visualizer-tool (%s) stonking; urgency=medium\n\n' "$debian_version"
        printf '  * New upstream release.\n\n'
        printf ' -- %s <%s>  %s\n\n' \
            "$maintainer_name" "$maintainer_email" "$(LC_ALL=C date -R)"
        cat "$repository_dir/debian/changelog"
    } > "$changelog_temp"
    mv "$changelog_temp" "$repository_dir/debian/changelog"
fi

if ! grep -F 'craftctl set version=' "$repository_dir/snapcraft.yaml" |
        grep -Fq '$CRAFT_PART_SRC/VERSION'; then
    echo "snapcraft.yaml must derive its package version from VERSION." >&2
    exit 1
fi

echo "PVT version is now $version. README, Debian, and Snapcraft metadata are synchronized; reconfigure the build to apply it."
