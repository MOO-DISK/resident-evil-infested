# RE1 Asset Migrator

A portable Qt GUI that migrates the assets this port needs. It replaces the old
`scripts/build_dc_assets.py` importer and adds the base-game migration the script
never handled.

It is a **tool**, not part of the game: `Game.vcxproj` and the root
`CMakeLists.txt` do not reference it, and it builds 64-bit with its own Qt kit
(the game stays 32-bit MSVC).

## Tabs

### PC Assets

Migrates a USA or Japanese base tree.

- **Source** is either an already-extracted folder or a disc image
  (`.iso` / `.bin` / `.cue`). From an image the asset folders are read straight
  out of ISO9660; from a folder they are copied.
- **Asset type** picks the destination tree, `<target>/USA` or `<target>/JPN`.
- Folder names are canonicalised on the way in (`DATA` -> `Data`,
  `MOVIE` -> `Movie`, ...), so a retail layout and this repo's tree both work.
- **Convert movies** transcodes every `Movie/*.avi` to `.mp4` (H.264 + AAC)
  with ffmpeg. The engine prefers a `.mp4` sibling and falls back to the `.avi`,
  so keeping the AVI is safe (and the default).

The base tree is only added to; existing files are never deleted.

### Director's Cut

Builds the `DC/` overlay from a Director's Cut disc image. **An image is
required**, not an extracted folder: a raw 2352-byte `.bin`/`.cue` keeps the
CD-XA audio of the `.STR` movies intact, which a 2048-byte content copy
truncates.

It extracts the disc's asset folders, runs every overlay step (arrange rooms,
rooms, models, Data, title art, item sprites/models, the save-screen font CLUT
rows and the PS1 colour-key -> PC index-0 transparency relabel), decodes **every**
`.BSS` background to `.pak` (STAGE1-7 and STAGE8-E), and converts the `.STR`
movies to `.mp4`.

`Base tree` selects which tree the overlay is built on top of (it is only read:
the font comes from it and the movie retiming looks up the shipped PC AVIs in
`<target>/USA/Movie` then `<target>/JPN/Movie`). `Verify` runs the coverage and
background checks afterwards.

## Requirements

- Qt 6.8 (MSVC 2022 64-bit kit; `C:\Qt\6.8.3\msvc2022_64` by default).
- Visual Studio 2022/2026 with the C++ toolset.
- `ffmpeg` on `PATH` (or browsed to) for the movie conversions. Everything else
  is self-contained; there is no Python dependency at runtime.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File tools/asset_migrator/build.ps1
```

or by hand:

```
cmake -S tools/asset_migrator -B tools/asset_migrator/build `
      -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build tools/asset_migrator/build --config Release
```

Produces `tools/asset_migrator/build/Release/re1_asset_migrator.exe`.

## Package a portable folder

```powershell
powershell -ExecutionPolicy Bypass -File tools/asset_migrator/build.ps1 -Package
```

This also runs `windeployqt` and copies the exe, the Qt DLLs and the platform
plugin into `tools/asset_migrator/dist/re1_asset_migrator/`, which runs on a
machine with no Qt installed. `ffmpeg.exe` is not bundled.

## Releases (CI)

`.github/workflows/release.yml` has a `build-asset-migrator` job that installs Qt
6.8.3 (`jurplel/install-qt-action`), builds the tool with the runner's VS 2022
toolset, deploys the Qt runtime and publishes
`asset-migrator-<version>-windows-x64.zip` as its own release asset (next to the
game's `residentevil-<version>-windows-x86.zip` and the Linux bundle). It runs on
every push to `main` as a compile check, and attaches the zip to the GitHub
release when release-please cuts one. ffmpeg is not bundled, so players need it
on `PATH` (or browsed to) for the movie conversions.

## Headless verification

`re1am_selftest.exe` (built alongside the GUI) exercises the core without Qt:

```
re1am_selftest info <image> [filter]
re1am_selftest bss <image> <path-in-image> <outdir>
re1am_selftest strinfo <image> [filter]
re1am_selftest pc <source> <target> <USA|JPN> [--image] [--movies]
re1am_selftest dc <image> <target> <USA|JPN> [--no-bg] [--no-movies] [--no-verify]
```

The C++ decoders were checked against the Python tools:

- `bss` -> `.pak` output is **byte-identical** to `tools/bss_to_pak.py` for
  every camera of a base-stage `.BSS` (ROOM100: 6/6 files match).
- A full DC overlay build (`--no-bg`) is **byte-identical** to
  `scripts/build_dc_assets.py` run over the same extracted disc: 584 files,
  0 differences, and the same 136 relabelled files / 3,372,204 texels.
- The `.STR` demux matches `tools/str_to_video.py` exactly (DM8: 160 frames,
  256x240, v2, 3582 audio groups) and the `--timing pc` retime reproduces its
  `11.20 s / -5.2%` result.
