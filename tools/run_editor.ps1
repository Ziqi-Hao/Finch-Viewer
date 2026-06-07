param(
    [string]$Fa = "SUBG08_tissue_FA_aggressive.nii.gz",
    [string]$Trk = "SUBG08_OR_full.trk",
    [string]$Out = "SUBG08_OR_edited.trk",
    [int]$DisplayN = 12000,
    [int]$DispStep = 2,
    [switch]$NoFa,
    [switch]$FrontBuffer,
    [string]$Screenshot = "",
    [switch]$BuildFirst
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Exe = Join-Path $RepoRoot "build\Release\local_editor_cpp.exe"

if ($BuildFirst -or !(Test-Path $Exe)) {
    & (Join-Path $PSScriptRoot "build_release.ps1") -SkipSmoke
}

$TrkPath = Join-Path $RepoRoot $Trk
$OutPath = Join-Path $RepoRoot $Out

if (!(Test-Path $TrkPath)) {
    throw "Missing TRK file: $TrkPath"
}

$ExeArgs = @(
    "--trk", $TrkPath,
    "--out", $OutPath,
    "--display-n", $DisplayN,
    "--disp-step", $DispStep
)

if ($NoFa) {
    $ExeArgs += "--no-fa"
} else {
    $FaPath = Join-Path $RepoRoot $Fa
    if (!(Test-Path $FaPath)) {
        throw "Missing FA file: $FaPath"
    }
    $ExeArgs = @("--fa", $FaPath) + $ExeArgs
}

if ($Screenshot -ne "") {
    $ExeArgs += @("--screenshot", (Join-Path $RepoRoot $Screenshot))
}

if ($FrontBuffer) {
    $ExeArgs += "--front-buffer"
}

& $Exe @ExeArgs
