# Build (and optionally package) the RE1 Asset Migrator.
#
#   powershell -ExecutionPolicy Bypass -File tools/asset_migrator/build.ps1
#   powershell -ExecutionPolicy Bypass -File tools/asset_migrator/build.ps1 -Package
#
# Qt 6.8 MSVC 2022 64-bit is expected at -QtDir (or $env:QTDIR). ffmpeg is a
# runtime dependency and is not bundled.
param(
    [string]$QtDir = $(if ($env:QTDIR) { $env:QTDIR } else { "C:\Qt\6.8.3\msvc2022_64" }),
    [string]$Config = "Release",
    [switch]$Package
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"

function Find-CMake {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        if ($vs) {
            $c = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $c) { return $c }
        }
    }
    throw "cmake not found (install CMake or Visual Studio's C++ CMake tools)"
}

if (-not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6\Qt6Config.cmake"))) {
    throw "Qt 6 not found at $QtDir (pass -QtDir or set QTDIR)"
}

$cmake = Find-CMake
Write-Host "cmake : $cmake"
Write-Host "Qt    : $QtDir"

& $cmake -S $root -B $build -G "Visual Studio 18 2026" -A x64 `
    "-DCMAKE_PREFIX_PATH=$($QtDir -replace '\\','/')"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $build "$Config\re1_asset_migrator.exe"
Write-Host "built : $exe"

if ($Package) {
    $windeploy = Join-Path $QtDir "bin\windeployqt.exe"
    if (-not (Test-Path $windeploy)) { throw "windeployqt not found at $windeploy" }
    & $windeploy --release --no-translations --no-system-d3d-compiler --no-opengl-sw $exe
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

    $dist = Join-Path $root "dist\re1_asset_migrator"
    if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Copy-Item (Join-Path $build "$Config\*") $dist -Recurse -Force
    # The import library / linker artifacts are build outputs, not runtime.
    Remove-Item (Join-Path $dist "*.lib"), (Join-Path $dist "*.exp"),
        (Join-Path $dist "*.pdb") -Force -ErrorAction SilentlyContinue
    Write-Host "packaged: $dist"
}
