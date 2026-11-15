<#
.SYNOPSIS
    Regenerates every Blender-made source asset under assets_src (headless, deterministic).

.DESCRIPTION
    Runs the generators in dependency order with Blender's bundled Python:
      gen_textures.py  -> assets_src/textures/terrain/*.png
      gen_props.py     -> assets_src/props/*.glb
      gen_character.py -> assets_src/characters/{aydor,kargan}.glb + .anims.toml
      gen_terrain.py   -> assets_src/terrain/zone1/* (its preview imports the props)
    Each also writes preview renders to assets_src/previews/. Never touches a running Blender
    session (always --background --factory-startup). Windows PowerShell 5.1 compatible.

.PARAMETER Blender
    Path to blender.exe. Default: $env:VO_BLENDER, then the standard Blender 5.3 install path.

.PARAMETER NoPreview
    Skip the preview renders (faster; the sources are identical).

.EXAMPLE
    powershell -File tools/blender/build_all.ps1
#>
[CmdletBinding()]
param(
    [string]$Blender = '',
    [switch]$NoPreview
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
if (-not $Blender) {
    if ($env:VO_BLENDER) { $Blender = $env:VO_BLENDER }
    else { $Blender = Join-Path $env:ProgramFiles 'Blender Foundation\Blender 5.3\blender.exe' }
}
if (-not (Test-Path $Blender)) {
    Write-Host "Blender not found at '$Blender' (use -Blender or set VO_BLENDER)." -ForegroundColor Red
    exit 1
}

$steps = @(
    @{ Script = 'gen_textures.py'; Args = @() },
    @{ Script = 'gen_props.py'; Args = @() },
    @{ Script = 'gen_character.py'; Args = @('--race', 'aydor') },
    @{ Script = 'gen_character.py'; Args = @('--race', 'kargan') },
    @{ Script = 'gen_terrain.py'; Args = @() }
)

$started = Get-Date
foreach ($step in $steps) {
    $scriptArgs = @($step.Args)
    if ($NoPreview) { $scriptArgs += '--no-preview' }
    $label = "$($step.Script) $($scriptArgs -join ' ')"
    Write-Host "==> $label" -ForegroundColor Cyan
    $blenderArgs = @('--background', '--factory-startup', '--python-exit-code', '1',
        '--python', (Join-Path $Here $step.Script), '--') + $scriptArgs
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # Blender writes progress to stderr
    try {
        $output = & $Blender @blenderArgs 2>&1
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $saved
    }
    foreach ($line in $output) {
        $text = "$line"
        if ($text -match '^(STATS|SLIDE|PROP|TEXTURE|TERRAIN|SPAWN) ' -or $text -match 'Error|Traceback') {
            Write-Host "   $text"
        }
    }
    if ($code -ne 0) {
        Write-Host "FAILED: $label (exit $code)" -ForegroundColor Red
        $output | Select-Object -Last 30 | ForEach-Object { Write-Host "   $_" }
        exit 1
    }
}
Write-Host ("==> All sources regenerated in {0:N0} s" -f ((Get-Date) - $started).TotalSeconds) -ForegroundColor Cyan
exit 0
