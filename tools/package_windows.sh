#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# package_windows.sh - cross-compile the Windows x86 build on Linux and zip it.
#
# Builds residentevil.exe with MinGW-w64 (cmake/mingw-i686.cmake,
# docs/WINDOWS_CROSS_BUILD.md) and stages the same files as the release
# workflow's Windows zip, minus the .pdb (GCC writes no PDB), plus the zombie
# mod's infested/ files (tools/stage_infested.sh).
#
# Usage (from the repo root):
#   bash tools/package_windows.sh [--no-build] [--out DIR]
#
# Output: <out>/residentevil-<version>-windows-x86.zip, holding
#   residentevil.exe, config.ini (from config.ini.template), README.md, LICENSE,
#   infested/
#
# Needs: cmake, zip, and the i686 MinGW-w64 C++ compiler
#   Debian/Ubuntu: g++-mingw-w64-i686     Arch/CachyOS: mingw-w64-gcc
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO/build/windows"
OUT_ROOT="$REPO/dist"
DO_BUILD=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --out)      OUT_ROOT="$(realpath -m "$2")"; shift ;;
        -h|--help)  sed -n '3,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$DO_BUILD" = 1 ]; then
    cmake -S "$REPO" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/mingw-i686.cmake" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

EXE="$BUILD_DIR/residentevil.exe"
[ -f "$EXE" ] || { echo "no $EXE - build first" >&2; exit 1; }

VERSION="$(grep -m1 'GAME_VERSION_STRING' "$REPO/src/Version.h" | sed 's/.*"\([^"]*\)".*/\1/')"
[ -n "$VERSION" ] || VERSION="unknown"
NAME="residentevil-$VERSION-windows-x86"
STAGE="$OUT_ROOT/$NAME"

rm -rf "$STAGE" "$OUT_ROOT/$NAME.zip"
mkdir -p "$STAGE"
cp "$EXE" "$STAGE/"
# config.ini is runtime state the game rewrites; ship the template.
cp "$REPO/config.ini.template" "$STAGE/config.ini"
cp "$REPO/README.md" "$REPO/LICENSE" "$STAGE/"
bash "$REPO/tools/stage_infested.sh" "$STAGE"
(cd "$STAGE" && zip -q -r "../$NAME.zip" .)

echo "$OUT_ROOT/$NAME.zip"
