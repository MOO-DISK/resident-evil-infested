# Building the Windows executable on Linux

The Win32 game can be cross-compiled on Linux with MinGW-w64, so a Windows
release does not need a Windows machine. `Game.vcxproj` / `build.bat` (MSVC)
stay the reference Windows build and what CI ships.

## Requirements

- CMake 3.16+ and `zip`
- the i686 MinGW-w64 C++ compiler:
  - Debian/Ubuntu: `sudo apt install g++-mingw-w64-i686`
  - Arch/CachyOS: `sudo pacman -S mingw-w64-gcc`

## Build

```
cmake -S . -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows -j
```

Output: `build/windows/residentevil.exe`, with `infested/` copied beside it
as the MSVC build does.

For a release zip (the same files as the workflow's Windows zip, without the
`.pdb`): `bash tools/package_windows.sh` → `dist/residentevil-<version>-windows-x86.zip`.

## How it works

- **Sources:** `CMakeLists.txt` reads the `<ClCompile>` list out of
  `Game.vcxproj`, so adding a file to the Windows build still means adding it
  to the vcxproj only.
- **Libraries:** the vcxproj's plus the ones its sources pull in with
  `#pragma comment(lib)`, which GCC ignores. Everything (libstdc++, libgcc,
  winpthread) is linked statically; the exe imports only Windows system DLLs.
  XAudio2 comes from `XAudio2_8.dll` (MinGW ships no 2.9 import library);
  Windows 8 and later have it.
- **Task scheduler:** MinGW has no `ucontext`, and GCC cannot compile MSVC
  `__declspec(naked)`/`__asm` blocks. `TaskScheduler.cpp` carries the same
  five switch routines as GNU file-level Intel-syntax asm (`TASK_SWITCH_ASM`).
- **rand/srand:** as on Linux, `rand`/`srand` are defined to forward to
  `re1_rand`/`re1_srand` instead of being macros (libstdc++ names `std::rand`).

## Differences from the MSVC build

- No `.pdb`; `crash.log` symbolization through dbghelp finds no symbols, so
  crash addresses are raw. Keep the matching `residentevil.exe` to look them
  up (`i686-w64-mingw32-addr2line -f -e residentevil.exe <addr>`, after a
  build with `-DCMAKE_BUILD_TYPE=RelWithDebInfo`).
- The C runtime is `msvcrt.dll`, not the static UCRT.
- A different compiler: code relying on undefined behaviour or MSVC-specific
  layout can act differently. Play-test a MinGW build before publishing it.
