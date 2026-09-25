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

**Add PS1 assets from a PS1 disc image** (optional) supplements the same tree
from a 1996 PS1 disc or the Director's Cut, for the PS1 staff and cast rolls in
OG mode (`[Game] Ps1EndingCredits=1`):

- **Ending-credit data** copies `DATA/STAFF.STF`, `STAFF2.STF` and `BIO.TIM`
  into `Data/`. The tree's own `EN05`/`EN07`/`CLIS01`/`JILL01` are kept.
- **Convert PS1 movies** turns the disc's `.STR` files into `.mp4` in `Movie/`:
  always `STFC`/`STFJ` (the movies the PS1 credits play), plus any movie the
  tree has no `.avi`/`.mp4` of. Movies are named the way the PC FMV table
  expects (USA: `ED4`->`EU4`, `ED5`->`EU5`, `OJ`->`OU`, `PJ`->`PU`); `STFC`/`STFJ`
  keep their names, since `stfc_r`/`stfj_r` are the PC rolls with the credits
  baked in.
- **Replace the tree's own movies** overwrites those with the PS1 versions
  (and re-converts `STFC`/`STFJ`).

With a PS1 image the PC source can be left empty to update an existing tree.
Use a raw `.bin`/`.cue` so the movies' CD-XA audio is intact.

### Director's Cut

Builds the `DC/` overlay from a Director's Cut disc image. **An image is
required**, not an extracted folder: a raw 2352-byte `.bin`/`.cue` keeps the
CD-XA audio of the `.STR` movies intact, which a 2048-byte content copy
truncates.

It extracts the disc's asset folders, runs every overlay step (arrange rooms,
rooms, models, Data, title art, item sprites/models, the save-screen font CLUT
rows, the PS1 colour-key -> PC index-0 transparency relabel, and the
STAFF.STF/STAFF2.STF/BIO.TIM ending-credit resources), decodes **every**
`.BSS` background to `.pak` (STAGE1-7 and STAGE8-E), and converts the `.STR`
movies to `.mp4`.

`Base tree` selects which tree the overlay is built on top of (it is only read:
the font comes from it). Movie conversion keeps each STR's original CD-sector
timing by default, at the PS1's 150 sectors per second through the last valid
video sector. `Verify` runs
the coverage and background checks afterwards.

## Requirements

- Qt 6.8 (MSVC 2022 64-bit kit; `C:\Qt\6.8.3\msvc2022_64` by default).
- Visual Studio 2022/2026 with the C++ toolset.
- `ffmpeg` on `PATH` (or browsed to) for the movie conversions.

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
re1am_selftest pc <source|-> <target> <USA|JPN> [--image] [--movies]
               [--ps1 <image>] [--no-ps1-credits] [--no-ps1-movies] [--ps1-replace]
re1am_selftest dc <image> <target> <USA|JPN> [--no-bg] [--no-movies] [--no-verify]
```

The C++ decoders were checked against the Python tools:

- `bss` -> `.pak` output is **byte-identical** to `tools/bss_to_pak.py` for
  every camera of a base-stage `.BSS` (ROOM100: 6/6 files match).
- A full DC overlay build (`--no-bg`) is **byte-identical** to
  `scripts/build_dc_assets.py` for the shared output: 584 files,
  0 differences, and the same 136 relabelled files / 3,372,204 texels.
  The ending-credit step additionally emits `Data/staff.stf`,
  `Data/staff2.stf`, `Data/bio.tim`, `Data/en05.tim`, `Data/en07.tim`,
  `Data/clis01.pix`, and `Data/jill01.pix`.
- The `.STR` demux matches `tools/str_to_video.py` exactly (DM8: 160 frames,
  256x240, v2, 3582 audio groups).
