param()

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $user = [Environment]::GetEnvironmentVariable("Path", "User")
    $env:Path = "$machine;$user;$env:Path"
}

function Resolve-Exe {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [string[]]$Fallbacks = @()
    )

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    foreach ($candidate in $Fallbacks) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "Could not find $Name"
}

Refresh-Path

Write-Host "Repo: $RepoRoot"

$cmake = Resolve-Exe "cmake.exe" @("C:\Program Files\CMake\bin\cmake.exe")
Write-Host "`n[CMake]"
& $cmake --version

$vcpkg = "C:\vcpkg\vcpkg.exe"
Write-Host "`n[vcpkg]"
if (!(Test-Path $vcpkg)) {
    throw "Missing $vcpkg"
}
& $vcpkg version
& $vcpkg list vtk

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
Write-Host "`n[MSVC]"
if (!(Test-Path $vswhere)) {
    throw "Missing vswhere.exe"
}

$env:Path = "$(Split-Path $vswhere);$env:Path"

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (!$vsPath) {
    throw "Visual Studio C++ Build Tools were not found"
}

$vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
if (!(Test-Path $vsDevCmd)) {
    throw "Missing $vsDevCmd"
}

$checkCl = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && where cl"
cmd.exe /d /s /c $checkCl

Write-Host "`n[Build output]"
$exe = Join-Path $RepoRoot "build\Release\local_editor_cpp.exe"
if (Test-Path $exe) {
    Write-Host "Found $exe"
    & $exe --help
} else {
    Write-Host "No built executable yet."
}
