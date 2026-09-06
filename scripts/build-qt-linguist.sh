#!/usr/bin/env bash
# Companion modules for the source-built Linux ARM64 Qt used by desktop CI.
set -euo pipefail
qt_version="$1"
qt_prefix="$2"
qt_work="$3"
if [[ "$qt_version" != "6.8.3" ]]; then
    echo "Update the pinned Linguist/Translations archive hashes for Qt $qt_version" >&2
    exit 1
fi
for module in qttools qttranslations; do
    archive="$qt_work/$module-$qt_version.tar.xz"
    source_dir="$qt_work/$module-$qt_version"
    if [[ "$module" == qttools ]]; then
        checksum=02a4e219248b94f1333df843d25763f35251c1074cdc4fb5bda67d340f8c8b3a
    else
        checksum=c3c61d79c3d8fe316a20b3617c64673ce5b5519b2e45535f49bee313152fa531
    fi
    curl --fail --location --retry 3 --output "$archive" \
        "https://download.qt.io/archive/qt/6.8/$qt_version/submodules/$module-everywhere-src-$qt_version.tar.xz"
    echo "$checksum  $archive" | sha256sum --check --strict
    mkdir -p "$source_dir"
    tar -xJf "$archive" --strip-components=1 -C "$source_dir"
    options=()
    if [[ "$module" == qttools ]]; then
        options+=(-DFEATURE_linguist=ON -DFEATURE_clang=OFF -DFEATURE_clangcpp=OFF)
        for feature in assistant designer distancefieldgenerator kmap2qmap pixeltool qdbus qev qdoc qtattributionsscanner qtdiag qtplugininfo; do
            options+=("-DFEATURE_$feature=OFF")
        done
    fi
    cmake -S "$source_dir" -B "$qt_work/$module-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$qt_prefix" \
        -DCMAKE_PREFIX_PATH="$qt_prefix" -DQT_BUILD_EXAMPLES=OFF \
        -DQT_BUILD_TESTS=OFF "${options[@]}"
    cmake --build "$qt_work/$module-build" --parallel
    cmake --install "$qt_work/$module-build"
done
