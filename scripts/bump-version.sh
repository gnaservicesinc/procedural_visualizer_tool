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
    # Keep the replacement beside its source so mv remains an atomic rename,
    # and seed it with the source metadata so mktemp's 0600 mode is not leaked
    # into README/manpage/package inputs.
    rewrite_temp=$(mktemp "${rewrite_source}.tmp.XXXXXX")
    if ! cp -p "$rewrite_source" "$rewrite_temp"; then
        rm -f "$rewrite_temp"
        return 1
    fi
    if ! sed -E "$rewrite_expression" "$rewrite_source" > "$rewrite_temp"; then
        rm -f "$rewrite_temp"
        return 1
    fi
    mv "$rewrite_temp" "$rewrite_source"
}

if ! grep -F 'craftctl set version=' "$repository_dir/snapcraft.yaml" |
        grep -Fq '$CRAFT_PART_SRC/VERSION'; then
    echo "snapcraft.yaml must derive its package version from VERSION." >&2
    exit 1
fi

version_file="$repository_dir/VERSION"
version_temp=$(mktemp "${version_file}.tmp.XXXXXX")
if ! cp -p "$version_file" "$version_temp"; then
    rm -f "$version_temp"
    exit 1
fi
printf '%s\n' "$version" > "$version_temp"
mv "$version_temp" "$version_file"
rewrite_file "$repository_dir/README.md" \
    "s/(Current product version: \\*\\*)[^*]+(\\*\\*)/\\1${version}\\2/"
rewrite_file "$repository_dir/debian/pvt-render.1" \
    '1s/(Procedural Visualizer Tool )[^\"]+(\" \"User Commands)/\1'"${version}"'\2/'
rewrite_file "$repository_dir/debian/procedural-visualizer-tool.1" \
    '1s/(Procedural Visualizer Tool )[^\"]+(\" \"User Commands)/\1'"${version}"'\2/'

abi_major=${version%%.*}
runtime_package="libproceduralvisualizertool${abi_major}"
control_file="$repository_dir/debian/control"
current_runtime_package=$(sed -n \
    's/^Package: \(libproceduralvisualizertool[0-9][0-9]*\)$/\1/p' \
    "$control_file")
case "$current_runtime_package" in
    ""|*'
'*)
        echo "debian/control must declare exactly one versioned PVT runtime package." >&2
        exit 1
        ;;
esac
current_runtime_install="$repository_dir/debian/${current_runtime_package}.install"
runtime_install="$repository_dir/debian/${runtime_package}.install"
if [ ! -f "$current_runtime_install" ]; then
    echo "Missing Debian install manifest: $current_runtime_install" >&2
    exit 1
fi
if [ "$current_runtime_package" != "$runtime_package" ]; then
    rewrite_file "$control_file" \
        "s/${current_runtime_package}/${runtime_package}/g"
    mv "$current_runtime_install" "$runtime_install"
fi
rewrite_file "$runtime_install" \
    's/(libProceduralVisualizerTool\.so\.)[0-9]+(\*)/\1'"${abi_major}"'\2/'

debian_version=$(printf '%s\n' "$version" | sed 's/-/~/')
if ! head -n 1 "$repository_dir/debian/changelog" |
        grep -Fq "procedural-visualizer-tool ($debian_version)"; then
    maintainer_name=${DEBFULLNAME:-$(git -C "$repository_dir" config user.name || true)}
    maintainer_email=${DEBEMAIL:-$(git -C "$repository_dir" config user.email || true)}
    : "${maintainer_name:=PVT Maintainers}"
    : "${maintainer_email:=noreply@example.com}"
    changelog_source="$repository_dir/debian/changelog"
    changelog_temp=$(mktemp "${changelog_source}.tmp.XXXXXX")
    if ! cp -p "$changelog_source" "$changelog_temp"; then
        rm -f "$changelog_temp"
        exit 1
    fi
    {
        printf 'procedural-visualizer-tool (%s) stonking; urgency=medium\n\n' "$debian_version"
        printf '  * New upstream release.\n\n'
        printf ' -- %s <%s>  %s\n\n' \
            "$maintainer_name" "$maintainer_email" "$(LC_ALL=C date -R)"
        cat "$changelog_source"
    } > "$changelog_temp"
    mv "$changelog_temp" "$changelog_source"
fi

echo "PVT version is now $version. README, Debian, and Snapcraft metadata are synchronized; reconfigure the build to apply it."
