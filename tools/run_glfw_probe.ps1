param(
    [switch]$BuildFirst
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build"
$Exe = Join-Path $BuildDir "Release\glfw_probe.exe"

if ($BuildFirst -or !(Test-Path $Exe)) {
    & (Join-Path $PSScriptRoot "build_release.ps1") -SkipSmoke
}

if (!(Test-Path $Exe)) {
    throw "Missing GLFW probe executable: $Exe"
}

& $Exe
