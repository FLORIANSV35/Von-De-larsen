#!/bin/bash
# Builds the macOS installer (.pkg): standalone app, AU, VST3 and the Neural Engine model.
#   tools/make_installer.sh [version]
# Environment: MODEL (compiled .mlmodelc folder, default: the one installed for the current user),
#              BUILD (CMake build folder), OUT (where the .pkg goes, default dist/).
# The package is not signed (no Developer ID): macOS asks for a right click > Open the first time.
set -euo pipefail
export COPYFILE_DISABLE=1

VERSION="${1:-0.2.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-installer}"
OUT="${OUT:-$ROOT/dist}"
MODEL="${MODEL:-$HOME/Library/Application Support/VonDeLarsen/denoise.mlmodelc}"
[ -f "$MODEL/model.mil" ] || { echo "No compiled model at: $MODEL (set MODEL=...)" >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD" -DVDL_BUILD_PLUGIN=ON -DVDL_BUILD_STANDALONE=ON -DVDL_BUILD_TESTS=OFF > /dev/null
cmake --build "$BUILD" --config Release -j "$(sysctl -n hw.ncpu)" > /dev/null

find_one() { find "$BUILD" -name "$1" -maxdepth 6 -print -quit; }
APP="$(find_one 'Von De Larsen.app')"; AU="$(find_one 'Von De Larsen.component')"; VST3="$(find_one 'Von De Larsen.vst3')"
for f in "$APP" "$AU" "$VST3"; do [ -n "$f" ] || { echo "Build output missing" >&2; exit 1; }; done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
stage() { # stage <name> <destination folder in the payload> <source>...
    local name="$1" dest="$2"; shift 2
    mkdir -p "$WORK/$name/root/$dest"
    for src in "$@"; do ditto --norsrc --noextattr --noacl "$src" "$WORK/$name/root/$dest/$(basename "$src")"; done
}
# The build signs the bundles before its last resources are in place (codesign --verify fails): sign again, ad hoc.
resign() { codesign --force --deep --sign - "$1" 2> /dev/null && codesign --verify --deep --strict "$1"; }
stage app  "Applications" "$APP"
stage au   "Library/Audio/Plug-Ins/Components" "$AU"
stage vst3 "Library/Audio/Plug-Ins/VST3" "$VST3"
stage model "Library/Application Support/VonDeLarsen" "$MODEL"
resign "$WORK/app/root/Applications/Von De Larsen.app"
resign "$WORK/au/root/Library/Audio/Plug-Ins/Components/Von De Larsen.component"
resign "$WORK/vst3/root/Library/Audio/Plug-Ins/VST3/Von De Larsen.vst3"

pkg() { # pkg <name> <identifier> [scripts folder]
    local scripts=()
    [ -n "${3:-}" ] && scripts=(--scripts "$3")
    # a bundle must land exactly where we say, not wherever an older copy happens to be
    pkgbuild --analyze --root "$WORK/$1/root" "$WORK/$1.plist" > /dev/null
    for i in $(seq 0 $(( $(/usr/libexec/PlistBuddy -c Print "$WORK/$1.plist" | grep -c '^    Dict') - 1 ))); do
        /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$WORK/$1.plist" 2> /dev/null || true
    done
    pkgbuild --root "$WORK/$1/root" --component-plist "$WORK/$1.plist" --identifier "$2" --version "$VERSION" \
        --ownership recommended ${scripts[@]+"${scripts[@]}"} "$WORK/$1.pkg" > /dev/null
}
pkg app   com.vondelarsen.app
pkg au    com.vondelarsen.au "$ROOT/tools/installer/scripts-au"
pkg vst3  com.vondelarsen.vst3
pkg model com.vondelarsen.model

mkdir -p "$OUT"
sed "s/@VERSION@/$VERSION/g" "$ROOT/tools/installer/distribution.xml" > "$WORK/distribution.xml"
productbuild --distribution "$WORK/distribution.xml" --resources "$ROOT/tools/installer/resources" \
    --package-path "$WORK" "$OUT/VonDeLarsen-$VERSION.pkg" > /dev/null
echo "Installer: $OUT/VonDeLarsen-$VERSION.pkg"
