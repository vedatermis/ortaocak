<#
.SYNOPSIS
    Regenerates every Blender-made source asset under assets_src (headless, deterministic).

.DESCRIPTION
    Runs the generators in dependency order with Blender's bundled Python:
      gen_textures.py  -> assets_src/textures/terrain/*.png
      gen_props.py     -> assets_src/props/*.glb
      gen_weapons.py   -> assets_src/weapons/*.glb + maps, assets_src/ui/items/*.png (inventory icons)
      gen_character.py -> assets_src/characters/<race>_<n>.glb + .png + .anims.toml, n = 0..2 (rigged
                          source meshes), plus the legacy <race>.glb/.anims.toml copies of look 0
      gen_monsters.py  -> assets_src/monsters/<model>.glb + maps + .anims.toml (rigged monsters)
      gen_effects.py   -> assets_src/effects/ember_bolt.glb, assets_src/ui/skills/*.png (M9 four)
      gen_skill_art.py -> M12: 24 branch skill icons, assets_src/ui/status/*.png, assets_src/ui/skillbar/*.png
                          (contact sheets in build/art_sheets/m12, not assets)
      gen_skill_effects.py -> M12: assets_src/effects/*.glb skill effects (+ build/art_sheets/m12/m12_effects.png)
      gen_loot.py      -> assets_src/props/loot_bag.glb + maps, loot item icons in assets_src/ui/items/
                          (materials, coin, weapon variants re-rendered from assets_src/weapons/*.glb)
      gen_attachments.py -> gamedata/attachments.toml (weapon in hand, per grip / look / hand)
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
    @{ Script = 'gen_weapons.py'; Args = @() },
    @{ Script = 'gen_character.py'; Args = @('--look', 'aydor_0') },
    @{ Script = 'gen_character.py'; Args = @('--look', 'aydor_1') },
    @{ Script = 'gen_character.py'; Args = @('--look', 'aydor_2') },
    @{ Script = 'gen_character.py'; Args = @('--look', 'kargan_0') },
    @{ Script = 'gen_character.py'; Args = @('--look', 'kargan_1') },
    @{ Script = 'gen_character.py'; Args = @('--look', 'kargan_2') },
    @{ Script = 'gen_monsters.py'; Args = @() },
    @{ Script = 'gen_effects.py'; Args = @() },
    @{ Script = 'gen_skill_art.py'; Args = @() },
    @{ Script = 'gen_skill_effects.py'; Args = @() },
    @{ Script = 'gen_loot.py'; Args = @() },
    @{ Script = 'gen_attachments.py'; Args = @() },
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
        if ($text -match '^(STATS|SLIDE|BIND|WALK|LEGS|SIT|RIGID|PROP|WEAPON|ATTACH|TEXTURE|EFFECT|SKILL|ICON|TERRAIN|SPAWN) ' -or $text -match 'Error|Traceback') {
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
