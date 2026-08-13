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
printf '%s\n' "$1" > "$repository_dir/VERSION"
echo "PVT version is now $1. Reconfigure the build to apply it."
