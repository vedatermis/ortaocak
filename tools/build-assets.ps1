<#
.SYNOPSIS
    Compiles assets_src into the runtime asset tree <repo>/assets (git-ignored) with assetc.

.DESCRIPTION
    1. Builds assetc first (build.ps1 -Target assetc: an incremental no-op when it is up to date, a
       rebuild when its sources changed), so a stale or stub assetc.exe is never used. -NoBuild skips
       this; the script then refuses an assetc.exe older than any of its sources.
    2. Runs `assetc build assets_src <Out>` (incremental; -Force rebuilds everything).
    3. Verifies the result instead of trusting the exit code alone: assetc must print its summary
       line with 0 failures and at least one asset, and the outputs the game needs must exist
       (zone 1 terrain, both race meshes, two weapon meshes, an inventory icon .vtex, the three
       monsters, the ember bolt, a skill icon .vtex, the loot bag, the coin and a loot icon .vtex). Any problem
       exits non-zero with a message.
    Windows PowerShell 5.1 compatible.

.EXAMPLE
    powershell -File tools/build-assets.ps1
    powershell -File tools/build-assets.ps1 -Config Release -Force
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$Force,
    [switch]$NoBuild,
    [string]$Out = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Assetc = Join-Path $Root ("build\x64-{0}\bin\assetc.exe" -f $Config.ToLowerInvariant())
if (-not $Out) { $Out = Join-Path $Root 'assets' }
$Required = @('terrain\zone1\zone1.vterrain', 'characters\aydor.vmesh', 'characters\kargan.vmesh',
    'weapons\sword_soldier.vmesh', 'weapons\shield_oak.vmesh', 'ui\items\sword_soldier.vtex',
    'monsters\ash_thrall.vmesh', 'monsters\slag_walker.vmesh', 'monsters\ember_warden.vmesh',
    'effects\ember_bolt.vmesh', 'ui\skills\koz_oku.vtex', 'props\loot_bag.vmesh', 'ui\items\coin.vtex',
    'ui\items\sword_charred.vtex')

function Fail([string]$Message) {
    Write-Host "ASSET BUILD FAILED: $Message" -ForegroundColor Red
    exit 1
}

if (-not $NoBuild) {
    Write-Host "==> building assetc ($Config, incremental)" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'build.ps1') -Config $Config -Target assetc
    if ($LASTEXITCODE -ne 0) { Fail "build.ps1 -Target assetc exited $LASTEXITCODE" }
}
if (-not (Test-Path $Assetc)) { Fail "$Assetc does not exist (build it, or drop -NoBuild)" }

# A stale assetc (older than its sources, e.g. a stub from an old build) must not run silently.
$exeTime = (Get-Item $Assetc).LastWriteTimeUtc
$sourceDirs = @('tools\assetc', 'shared\assets') | ForEach-Object { Join-Path $Root $_ } | Where-Object { Test-Path $_ }
$newer = @(Get-ChildItem -Path $sourceDirs -Recurse -File -Include *.cpp, *.h, CMakeLists.txt |
        Where-Object { $_.LastWriteTimeUtc -gt $exeTime })
if ($newer.Count -gt 0) {
    Fail ("$Assetc is older than its sources ($($newer[0].FullName) and $($newer.Count - 1) more); " +
        'run without -NoBuild or build assetc')
}

$assetcArgs = @('build', (Join-Path $Root 'assets_src'), $Out)
if ($Force) { $assetcArgs += '--force' }
Write-Host "==> $Assetc $($assetcArgs -join ' ')" -ForegroundColor Cyan
$saved = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $output = @(& $Assetc @assetcArgs 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $saved
}
$output | ForEach-Object { Write-Host $_ }
if ($code -ne 0) { Fail "assetc exited $code" }

$summary = $output | Select-String -Pattern 'assetc build: (\d+) converted, (\d+) copied, (\d+) up to date, (\d+) failed' |
    Select-Object -Last 1
if ($null -eq $summary) { Fail 'assetc printed no summary line (not a real assetc build?)' }
$g = $summary.Matches[0].Groups
$total = [int]$g[1].Value + [int]$g[2].Value + [int]$g[3].Value
if ([int]$g[4].Value -ne 0) { Fail "$($g[4].Value) asset(s) failed" }
if ($total -eq 0) { Fail 'assetc processed no assets at all' }
foreach ($r in $Required) {
    $p = Join-Path $Out $r
    if (-not (Test-Path $p) -or (Get-Item $p).Length -eq 0) { Fail "required output $r is missing or empty" }
}
Write-Host "==> assets OK: $total asset(s) in $Out" -ForegroundColor Green
exit 0
