# Toolchain file: the Windows (Win32) build cross-compiled on Linux with
# MinGW-w64. See docs/WINDOWS_CROSS_BUILD.md.
#
#   cmake -S . -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/windows -j
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(RE1_MINGW_PREFIX i686-w64-mingw32 CACHE STRING "MinGW-w64 target triplet")
# Debian/Ubuntu ship both a -win32 and a -posix flavour; either works.
find_program(CMAKE_C_COMPILER   NAMES ${RE1_MINGW_PREFIX}-gcc ${RE1_MINGW_PREFIX}-gcc-win32 REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${RE1_MINGW_PREFIX}-g++ ${RE1_MINGW_PREFIX}-g++-win32 REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${RE1_MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${RE1_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
