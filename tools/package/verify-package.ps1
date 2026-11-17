<#
.SYNOPSIS
    Smoke-tests a package made by package.ps1 from a directory outside the repository: the packaged
    vo_server (temporary local config, fresh key pair, in-memory accounts, free 127.0.0.1 port) and
    the packaged vo_client (playing client/automation/flows/happy_path.voscript) must find their
    config/ and assets/ next to the executables and work together.

.DESCRIPTION
    Copies dist/VedatOnline-<version> to a temporary directory (outside the repo), writes
    server/config/server.local.toml there, runs `vo_server --gen-keys` in the copy, points the copied
    client's config/client.toml at it, then starts both executables WITHOUT --config and with the
    working directory set to the temporary root, so nothing can be found by searching upwards from
    the repository. Never touches config/ of the checkout, the user's client settings or any
    database. Fails if the client script fails, a log reports errors, or a process is left running.
    Windows PowerShell 5.1 compatible.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\package\verify-package.ps1 -Package dist\VedatOnline-0.1.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$WorkDir = '',
    [string]$Script = '',
    [int]$TimeoutSec = 240
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Package = (Resolve-Path $Package).Path
if (-not $Script) { $Script = Join-Path $Root 'client\automation\flows\happy_path.voscript' }
if (-not $WorkDir) { $WorkDir = Join-Path ([IO.Path]::GetTempPath()) ('vo_pkg_verify_' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
if ($WorkDir.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) { throw "WorkDir must be outside the repository" }

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $l.Start(); $port = $l.LocalEndpoint.Port; $l.Stop()
    return $port
}
function Read-Shared([string]$Path) {
    if (-not (Test-Path $Path)) { return '' }
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite, Delete')
    try { return (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Dispose() }
}

$serverProc = $null
$clientProc = $null
$exitCode = 1
try {
    Write-Step "copy $Package -> $WorkDir"
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    $copy = Join-Path $WorkDir (Split-Path -Leaf $Package)
    Copy-Item $Package $copy -Recurse
    $server = Join-Path $copy 'server'
    $client = Join-Path $copy 'client'
    $port = Get-FreePort

    # Local server overrides (what an operator would write), then a key pair next to the config.
    [IO.File]::WriteAllText((Join-Path $server 'config\server.local.toml'), @"
[net]
listen_address = "127.0.0.1"
listen_port = $port
[auth]
dev_auto_register = true
[log]
level = "debug"
file = '$($WorkDir.Replace('\','/'))/server.log'
[db]
connection_string = ""
"@, $Utf8)
    Write-Step 'vo_server --gen-keys (in the copy)'
    $gen = Start-Process -FilePath (Join-Path $server 'vo_server.exe') -ArgumentList '--gen-keys' -WorkingDirectory $WorkDir `
        -NoNewWindow -PassThru -Wait -RedirectStandardOutput (Join-Path $WorkDir 'genkeys.out.log') -RedirectStandardError (Join-Path $WorkDir 'genkeys.err.log')
    if ($gen.ExitCode -ne 0) { throw "--gen-keys exited $($gen.ExitCode): $(Read-Shared (Join-Path $WorkDir 'genkeys.err.log'))" }
    $pub = ([IO.File]::ReadAllText((Join-Path $server 'config\keys\server_ed25519.pub'))).Trim()

    # Point the copied client at it (host/port/key only).
    $clientToml = Join-Path $client 'config\client.toml'
    $text = [IO.File]::ReadAllText($clientToml)
    $text = [regex]::Replace($text, '(?m)^server_public_key = "[0-9a-fA-F]*"', "server_public_key = `"$pub`"")
    $text = [regex]::Replace($text, '(?m)^host = "[^"]*"', 'host = "127.0.0.1"')
    $text = [regex]::Replace($text, '(?m)^port = \d+', "port = $port")
    [IO.File]::WriteAllText($clientToml, $text, $Utf8)

    Write-Step "vo_server (cwd $WorkDir, no --config) on 127.0.0.1:$port"
    $serverProc = Start-Process -FilePath (Join-Path $server 'vo_server.exe') -ArgumentList '--run-seconds 300' `
        -WorkingDirectory $WorkDir -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $WorkDir 'server.out.log') -RedirectStandardError (Join-Path $WorkDir 'server.err.log')
    $null = $serverProc.Handle
    $deadline = (Get-Date).AddSeconds(20)
    $ready = $false
    while ((Get-Date) -lt $deadline -and -not $ready) {
        if ($serverProc.HasExited) { throw "vo_server exited early ($($serverProc.ExitCode)): $(Read-Shared (Join-Path $WorkDir 'server.out.log'))" }
        try { $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port); $c.Close(); $ready = $true } catch { Start-Sleep -Milliseconds 100 }
    }
    if (-not $ready) { throw 'vo_server did not listen within 20 s' }

    $shots = Join-Path $WorkDir 'screenshots'
    New-Item -ItemType Directory -Force -Path $shots | Out-Null
    $clientLog = Join-Path $WorkDir 'client.log'
    $clientArgs = @('--script', "`"$Script`"", '--screenshot-dir', "`"$shots`"", '--log', "`"$clientLog`"",
        '--settings', "`"$(Join-Path $WorkDir 'client_settings.toml')`"", '--size', '1280x720') -join ' '
    Write-Step "vo_client (cwd $WorkDir, no --config): $(Split-Path -Leaf $Script)"
    $clientProc = Start-Process -FilePath (Join-Path $client 'vo_client.exe') -ArgumentList $clientArgs -WorkingDirectory $WorkDir -PassThru
    $null = $clientProc.Handle
    if (-not $clientProc.WaitForExit($TimeoutSec * 1000)) { throw "vo_client did not finish within $TimeoutSec s" }
    $clientProc.WaitForExit()
    $log = Read-Shared $clientLog
    $problems = @()
    if ($clientProc.ExitCode -ne 0) { $problems += "vo_client exit code $($clientProc.ExitCode)" }
    if ($log -notmatch 'script finished') { $problems += 'script did not finish' }
    if ($log -match '\[(ERROR|FATAL)\]\s+\[T\d+\]\s+([^\r\n]+)') { $problems += "client log error: $($Matches[2])" }
    $expectConfig = [regex]::Escape((Join-Path $client 'config\client.toml'))
    if ($log -notmatch "config: $expectConfig") { $problems += 'the client did not use the packaged config/client.toml' }
    if ($log -notmatch ('assets: root ' + [regex]::Escape((Join-Path $client 'assets')))) { $problems += 'the client did not use the packaged assets/' }
    if ($log -notmatch 'world: entered as') { $problems += 'the client never entered the world' }
    Start-Sleep -Milliseconds 1500 # the server flushes its log file once a second
    $serverLog = (Read-Shared (Join-Path $WorkDir 'server.log')) + (Read-Shared (Join-Path $WorkDir 'server.out.log'))
    if ($serverLog -notmatch ('world: zone 1 \([^)]*\) from ' + [regex]::Escape((Join-Path $server 'assets')))) { $problems += 'the server did not use the packaged terrain' }
    if ($serverLog -notmatch ('zones: \d+ zone\(s\) from ' + [regex]::Escape((Join-Path $server 'gamedata')))) { $problems += 'the server did not use the packaged gamedata/zones.toml' }
    if ($serverLog -notmatch 'entered zone \d+') { $problems += 'no character entered the world on the server' }
    if ($serverLog -notmatch ('items: \d+ template\(s\) from ' + [regex]::Escape((Join-Path $server 'gamedata')))) { $problems += 'the server did not use the packaged gamedata/items.toml' }
    if ($serverLog -notmatch ('monsters: \d+ template\(s\), \d+ spawn point\(s\), \d+ safe zone\(s\) from ' + [regex]::Escape((Join-Path $server 'gamedata')))) { $problems += 'the server did not use the packaged gamedata/monsters.toml' }
    if ($serverLog -notmatch ('loot: \d+ table\(s\) from ' + [regex]::Escape((Join-Path $server 'gamedata')))) { $problems += 'the server did not use the packaged gamedata/loot.toml' }
    if ($serverLog -notmatch ('skills: \d+ skill\(s\) in \d+ branch\(es\) from ' + [regex]::Escape((Join-Path $server 'gamedata')))) { $problems += 'the server did not use the packaged gamedata/skills.toml' }
    $count = @(Get-ChildItem $shots -Filter *.png).Count
    Write-Host "client exit $($clientProc.ExitCode), $count screenshot(s) in $shots"
    if ($problems.Count -gt 0) { throw ($problems -join '; ') }
    Write-Host "PACKAGE VERIFIED: $copy" -ForegroundColor Green
    $exitCode = 0
}
catch {
    Write-Host "VERIFY FAILED: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    foreach ($p in @($clientProc, $serverProc)) {
        if ($null -ne $p -and -not $p.HasExited) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $p.WaitForExit(10000) | Out-Null
        }
    }
}
exit $exitCode
