<#
.SYNOPSIS
    Compiles assets_src into the runtime asset tree <repo>/assets (git-ignored) with assetc.

.DESCRIPTION
    Uses build/x64-<config>/bin/assetc.exe, building it with build.ps1 first if it is missing.
    Conversion is incremental (up-to-date outputs are skipped); -Force rebuilds everything.
    Exits non-zero if any asset fails. Windows PowerShell 5.1 compatible.

.EXAMPLE
    powershell -File tools/build-assets.ps1
    powershell -File tools/build-assets.ps1 -Config Release -Force
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$Force,
    [string]$Out = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Assetc = Join-Path $Root ("build\x64-{0}\bin\assetc.exe" -f $Config.ToLowerInvariant())
if (-not $Out) { $Out = Join-Path $Root 'assets' }

if (-not (Test-Path $Assetc)) {
    Write-Host "==> assetc not built yet; building ($Config)" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'build.ps1') -Config $Config -Target assetc
    if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAILED' -ForegroundColor Red; exit 1 }
}

$assetcArgs = @('build', (Join-Path $Root 'assets_src'), $Out)
if ($Force) { $assetcArgs += '--force' }
Write-Host "==> $Assetc $($assetcArgs -join ' ')" -ForegroundColor Cyan
$saved = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $Assetc @assetcArgs
    $code = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $saved
}
if ($code -ne 0) {
    Write-Host "ASSET BUILD FAILED (assetc exit $code)" -ForegroundColor Red
    exit 1
}
exit 0
