<#
.SYNOPSIS
    Builds the distributable VedatOnline package: dist/VedatOnline-<version>/{client,server}.

.DESCRIPTION
    1. Release build (build.ps1 -Config Release) unless -SkipBuild.
    2. Runtime assets with tools/build-assets.ps1 (Release assetc, verified outputs) into a staging
       directory (never the repository's assets/).
    3. Layout:
         client/  vo_client.exe, assets/, config/client.toml (placeholder host + the server's public
                  key), gamedata/items.toml, README-TR.txt
         server/  vo_server.exe, vo_admin.exe, config/server.toml, config/server.local.toml.example,
                  gamedata/items.toml, db/migrations/, assets/terrain/zone1/zone1.vterrain, README-TR.txt
         THIRD_PARTY.md
       Both executables find config/, gamedata/ and assets/ next to themselves (they search upwards from the
       working directory, then from the executable directory).
    4. Safety check: the script FAILS if the package contains a keys/ directory, a *.local.toml, a
       private key file, or a database URL with a real-looking password.
    5. -Zip: also dist/VedatOnline-<version>.zip.
    Windows PowerShell 5.1 compatible.

.PARAMETER ServerPublicKey
    The game server's Ed25519 public key (64 hex characters, printed by vo_server --gen-keys), written
    into client/config/client.toml. Default: config/keys/server_ed25519.pub of this checkout. Not a
    secret; the private key is never read or copied.

.PARAMETER ServerHost
    Host name written into client/config/client.toml (default: a placeholder to be edited).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\package\package.ps1 -ServerPublicKey <hex> -Zip
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$OutDir = '',
    [string]$ServerHost = 'oyun.sunucu-adresi.example',
    [int]$ServerPort = 15001,
    [string]$ServerPublicKey = '',
    [switch]$SkipBuild,
    [switch]$Zip
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Bin = Join-Path $Root 'build\x64-release\bin'
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Utf8Bom = New-Object System.Text.UTF8Encoding($true)

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Fail([string]$Message) {
    Write-Host "PACKAGE FAILED: $Message" -ForegroundColor Red
    exit 1
}

# ---- Version, key ----
if (-not $Version) {
    $m = Select-String -Path (Join-Path $Root 'CMakeLists.txt') -Pattern '^\s*VERSION\s+(\d+\.\d+\.\d+)' | Select-Object -First 1
    if ($null -eq $m) { Fail 'cannot read the project version from CMakeLists.txt' }
    $Version = $m.Matches[0].Groups[1].Value
}
if (-not $ServerPublicKey) {
    $pubFile = Join-Path $Root 'config\keys\server_ed25519.pub'
    if (Test-Path $pubFile) { $ServerPublicKey = ([IO.File]::ReadAllText($pubFile)).Trim() }
}
if ($ServerPublicKey -notmatch '^[0-9a-fA-F]{64}$') {
    Fail 'no valid server public key: pass -ServerPublicKey <64 hex chars> (vo_server --gen-keys prints it)'
}
if (-not $OutDir) { $OutDir = Join-Path $Root 'dist' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$Name = "VedatOnline-$Version"
$Pkg = Join-Path $OutDir $Name
$Stage = Join-Path $OutDir "_stage-$Version"

try {
    # ---- Build ----
    if (-not $SkipBuild) {
        Write-Step 'Release build'
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'build.ps1') -Config Release
        if ($LASTEXITCODE -ne 0) { Fail "build.ps1 -Config Release exited $LASTEXITCODE" }
    }
    foreach ($exe in @('vo_client.exe', 'vo_server.exe', 'vo_admin.exe', 'assetc.exe')) {
        if (-not (Test-Path (Join-Path $Bin $exe))) { Fail "missing $Bin\$exe (build Release first)" }
    }

    Write-Step 'Runtime assets'
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    $stagedAssets = Join-Path $Stage 'assets'
    $assetArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'tools\build-assets.ps1'),
        '-Config', 'Release', '-Out', $stagedAssets)
    if ($SkipBuild) { $assetArgs += '-NoBuild' }
    & powershell @assetArgs
    if ($LASTEXITCODE -ne 0) { Fail 'tools/build-assets.ps1 failed' }

    # ---- Layout ----
    Write-Step "Layout $Pkg"
    if (Test-Path $Pkg) { Remove-Item -Recurse -Force $Pkg }
    $client = Join-Path $Pkg 'client'
    $server = Join-Path $Pkg 'server'
    foreach ($d in @("$client\config", "$client\gamedata", "$server\config", "$server\gamedata", "$server\db\migrations",
                     "$server\assets\terrain\zone1")) {
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }

    # Client: executable, all runtime assets (without assetc's incremental stamps), config, readme.
    Copy-Item (Join-Path $Bin 'vo_client.exe') $client
    Copy-Item $stagedAssets (Join-Path $client 'assets') -Recurse
    $stamps = Join-Path $client 'assets\.assetc'
    if (Test-Path $stamps) { Remove-Item -Recurse -Force $stamps }
    $clientToml = @"
# VedatOnline client configuration (package $Version). See README-TR.txt.

# The game server's Ed25519 public key (vo_server --gen-keys). The client only talks to a server
# that signs the handshake with the matching private key ("sunucu dogrulanamadi" otherwise).
server_public_key = "$($ServerPublicKey.ToLowerInvariant())"

[server]
host = "$ServerHost"   # the game server's address: replace the placeholder
port = $ServerPort
connect_timeout_ms = 10000
request_timeout_ms = 15000

[window]
width = 1280
height = 720
vsync = true

[graphics]
shadows = true
msaa = 4               # 1 (off), 2, 4, 8

[log]
level = "info"
file = "vo_client.log" # next to vo_client.exe
"@
    [IO.File]::WriteAllText((Join-Path $client 'config\client.toml'), $clientToml.Replace("`r`n", "`n").Replace("`n", "`r`n"), $Utf8)

    # Server: executables, committed config, an example for the local overrides, migrations, terrain.
    Copy-Item (Join-Path $Bin 'vo_server.exe') $server
    Copy-Item (Join-Path $Bin 'vo_admin.exe') $server
    Copy-Item (Join-Path $Root 'config\server.toml') (Join-Path $server 'config\server.toml')
    $localExample = @"
# Copy to config\server.local.toml and adjust (see README-TR.txt). Only the keys set here override
# config\server.toml. This file holds secrets (the database password): never share it.

[server]
name = "VedatOnline"

[auth]
dev_auto_register = false   # true only on a development server

[log]
level = "info"
# file = "logs/server.log"

[db]
# PostgreSQL connection (libpq URI). VEDAT_DB_URL (environment) overrides it. Add ?sslmode=require
# for a database on another machine. Empty = in-memory accounts (lost when the server stops).
connection_string = "postgresql://vedat:CHANGE_ME@localhost:5432/vedatonline"
# Optional maintenance connection (database "postgres"): only to CREATE DATABASE if it is missing.
# admin_connection_string = "postgresql://vedat:CHANGE_ME@localhost:5432/postgres"
"@
    [IO.File]::WriteAllText((Join-Path $server 'config\server.local.toml.example'), $localExample.Replace("`r`n", "`n").Replace("`n", "`r`n"), $Utf8)
    Get-ChildItem (Join-Path $Root 'db\migrations') -File | Copy-Item -Destination (Join-Path $server 'db\migrations')
    Copy-Item (Join-Path $stagedAssets 'terrain\zone1\zone1.vterrain') (Join-Path $server 'assets\terrain\zone1\zone1.vterrain')
    # Item templates (docs/PROTOCOL_ITEMS.md): the server refuses to start without them; the client
    # reads the same file for names, tooltips and models.
    foreach ($side in @($client, $server)) {
        Copy-Item (Join-Path $Root 'gamedata\items.toml') (Join-Path $side 'gamedata\items.toml')
    }

    foreach ($side in @(@{ Dir = $client; Template = 'client-README-TR.txt' }, @{ Dir = $server; Template = 'server-README-TR.txt' })) {
        $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $side.Template), $Utf8)
        $text = $text.Replace('{VERSION}', $Version).Replace('{HOST}', $ServerHost).Replace("`r`n", "`n").Replace("`n", "`r`n")
        [IO.File]::WriteAllText((Join-Path $side.Dir 'README-TR.txt'), $text, $Utf8Bom) # BOM: Notepad shows the Turkish letters
    }
    Copy-Item (Join-Path $Root 'THIRD_PARTY.md') $Pkg

    # ---- Safety check: no secrets in the package ----
    Write-Step 'Checking for secrets'
    $problems = @()
    $all = @(Get-ChildItem -Path $Pkg -Recurse -Force)
    foreach ($item in $all) {
        $rel = $item.FullName.Substring($Pkg.Length + 1)
        if ($item.PSIsContainer -and $item.Name -ieq 'keys') { $problems += "key directory: $rel" }
        if (-not $item.PSIsContainer) {
            if ($item.Name -like '*.local.toml') { $problems += "local config: $rel" }
            if ($item.Name -match '\.(key|pem|pfx|p12)$' -or $item.Name -like 'server_ed25519*') { $problems += "key file: $rel" }
            if ($item.Extension -in @('.toml', '.txt', '.md', '.example', '.sql')) {
                $text = [IO.File]::ReadAllText($item.FullName)
                foreach ($m in [regex]::Matches($text, 'postgres(?:ql)?://[^:/\s"'']+:([^@\s"'']+)@')) {
                    if ($m.Groups[1].Value -notin @('CHANGE_ME', 'SIFRE', '...')) { $problems += "database password in $rel" }
                }
                if ($text -match '(?im)^\s*(password|passwd)\s*=\s*"[^"]+"') { $problems += "password setting in $rel" }
                if ($text -match '-----BEGIN [A-Z ]*PRIVATE KEY-----') { $problems += "private key in $rel" }
            }
        }
    }
    if ($problems.Count -gt 0) {
        if (Test-Path $Pkg) { Remove-Item -Recurse -Force $Pkg }
        Fail ("secrets found, package removed:`n  " + ($problems -join "`n  "))
    }

    # ---- Summary, zip ----
    $files = @($all | Where-Object { -not $_.PSIsContainer })
    $bytes = ($files | Measure-Object -Property Length -Sum).Sum
    Write-Host ("{0}: {1} files, {2:N1} MB" -f $Pkg, $files.Count, ($bytes / 1MB))
    if ($Zip) {
        $zipPath = Join-Path $OutDir "$Name.zip"
        if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
        Write-Step "Zip $zipPath"
        Compress-Archive -Path $Pkg -DestinationPath $zipPath
    }
    Write-Host "PACKAGE OK: $Pkg" -ForegroundColor Green
}
finally {
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue }
}
exit 0
