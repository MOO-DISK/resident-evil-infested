#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# stage_infested.sh - copy the zombie mod's runtime files into a package.
#
# Usage: bash tools/stage_infested.sh <package dir>
#
# Writes <package dir>/infested/ with what the game reads beside the exe, laid
# out as the builds copy it (Game.vcxproj CopyInfestedAudio, CMakeLists.txt):
#   infested/infested.wav            title screen (TitleScreen.cpp)
#   infested/audio/{helicopter,survive}.wav, infested/video/
#                                    match bumpers (ZombieBumpers.cpp)
#   infested/voices/<character>/     voice lines, .wav/.ogg (ZombieVoice.cpp)
# infested/images/ is not copied: those are compiled into the exe.
# Taken from the repo, not a build folder, so stale build copies stay out.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/infested"
[ $# -eq 1 ] || { echo "usage: $0 <package dir>" >&2; exit 2; }
DST="$1/infested"

rm -rf "$DST"
mkdir -p "$DST"
cp "$SRC/audio/infested.wav" "$DST/infested.wav"
for sound in helicopter survive; do
    [ -f "$SRC/audio/$sound.wav" ] || continue
    mkdir -p "$DST/audio"
    cp "$SRC/audio/$sound.wav" "$DST/audio/"
done
[ -d "$SRC/video" ] && cp -r "$SRC/video" "$DST/"
if [ -d "$SRC/voices" ]; then
    (cd "$SRC" && find voices -type f \( -name '*.wav' -o -name '*.ogg' \) -print0 |
        while IFS= read -r -d '' f; do
            mkdir -p "$DST/$(dirname "$f")"
            cp "$f" "$DST/$f"
        done)
fi
