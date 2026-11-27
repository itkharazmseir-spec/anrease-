<#
.SYNOPSIS
    Adds this AndreasOnline checkout (and a configured GTA SA install, if
    found) to the local Windows Defender exclusion list, so Defender's
    real-time scanner stops re-flagging freshly-built AndreasOnline.asi /
    AndreasOnlineServer.exe on every rebuild.

.DESCRIPTION
    This does NOT disable Windows Defender, and does NOT whitelist these
    binaries on anyone else's machine - it only tells the Defender running
    on THIS PC to stop scanning these specific folders. See
    docs/DEPLOYMENT.md section 5 ("Antivirus / SmartScreen flags...") for
    why these binaries get flagged in the first place (unsigned +
    process-injecting + network-opening looks like a heuristic match for
    malware, even though the code is open in this repo).

    Run this from an elevated (Administrator) PowerShell prompt. It is
    idempotent - running it again just re-adds the same paths, which is a
    no-op if they're already excluded.

.PARAMETER GtaSaDir
    Optional path to your GTA San Andreas installation (where gta_sa.exe
    and scripts\AndreasOnline.asi live). If omitted, only this repo's own
    folder is excluded.
#>
param(
    [string]$GtaSaDir
)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent() `
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Error "Run this from an elevated (Administrator) PowerShell window - Add-MpPreference requires it."
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot

$pathsToExclude = @($repoRoot)
if ($GtaSaDir) {
    if (-not (Test-Path $GtaSaDir)) {
        Write-Warning "GtaSaDir '$GtaSaDir' does not exist - skipping it."
    } else {
        $pathsToExclude += $GtaSaDir
    }
}

foreach ($path in $pathsToExclude) {
    Write-Host "Adding Defender exclusion: $path"
    Add-MpPreference -ExclusionPath $path
}

Write-Host ""
Write-Host "Done. Current Defender folder exclusions:"
(Get-MpPreference).ExclusionPath
