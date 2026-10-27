#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# package_release.sh - build and package both releases on Linux.
#
# Usage (from the repo root):
#   bash tools/package_release.sh [--no-build] [--out DIR]
#
# Output in <out> (default dist/):
#   residentevil-<version>-linux-x86.tar.gz   tools/package_linux.sh's bundle
#   residentevil-<version>-windows-x86.zip    tools/package_windows.sh (MinGW)
# Both carry config.ini (from config.ini.template) and the zombie mod's
# infested/ files (tools/stage_infested.sh). Game data is not included.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="$REPO/dist"
PASS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) PASS+=(--no-build) ;;
        --out)      OUT_ROOT="$(realpath -m "$2")"; shift ;;
        -h|--help)  sed -n '3,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

echo "==> Linux"
bash "$REPO/tools/package_linux.sh" "${PASS[@]}" --out "$OUT_ROOT"
VERSION="$(grep -m1 'GAME_VERSION_STRING' "$REPO/src/Version.h" | sed 's/.*"\([^"]*\)".*/\1/')"
[ -n "$VERSION" ] || VERSION="unknown"
LINUX="residentevil-$VERSION-linux-x86"
# package_linux.sh writes no config.ini; ship the template, as the workflow does.
cp "$REPO/config.ini.template" "$OUT_ROOT/$LINUX/config.ini"
tar -C "$OUT_ROOT" -czf "$OUT_ROOT/$LINUX.tar.gz" "$LINUX"

echo "==> Windows"
WINDOWS_ZIP="$(bash "$REPO/tools/package_windows.sh" "${PASS[@]}" --out "$OUT_ROOT" | tail -n1)"

echo
echo "$OUT_ROOT/$LINUX.tar.gz"
echo "$WINDOWS_ZIP"
