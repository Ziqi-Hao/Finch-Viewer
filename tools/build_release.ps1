param(
    [switch]$SkipConfigure,
    [switch]$ForceConfigure,
    [switch]$SkipSmoke
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build"
$Toolchain = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
$Overlay = Join-Path $RepoRoot "vcpkg-overlays"

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $user = [Environment]::GetEnvironmentVariable("Path", "User")
    $env:Path = "$machine;$user;$env:Path"
}

function Resolve-CMake {
    $cmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $fallback = "C:\Program Files\CMake\bin\cmake.exe"
    if (Test-Path $fallback) {
        return $fallback
    }

    throw "Could not find cmake.exe"
}

Refresh-Path

$cmake = Resolve-CMake

if (!(Test-Path $Toolchain)) {
    throw "Missing vcpkg toolchain: $Toolchain"
}

if (!$SkipConfigure) {
    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if ($ForceConfigure -or !(Test-Path $cache)) {
        & $cmake -S $RepoRoot -B $BuildDir -A x64 `
            -DCMAKE_TOOLCHAIN_FILE="$Toolchain" `
            -DVCPKG_OVERLAY_PORTS="$Overlay"
    } else {
        & $cmake -S $RepoRoot -B $BuildDir
    }
}

& $cmake --build $BuildDir --config Release --parallel

$exe = Join-Path $BuildDir "Release\local_editor_cpp.exe"
if (!(Test-Path $exe)) {
    throw "Build completed but executable was not found: $exe"
}

Write-Host "Built: $exe"

if (!$SkipSmoke) {
    & $exe --help
}
