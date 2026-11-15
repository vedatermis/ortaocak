<#
.SYNOPSIS
    Starts the local dev server in a new console window, then the game client.

.DESCRIPTION
    Runs tools/dev/setup-dev.ps1 first if the keys or config/client.local.toml are missing. The server
    uses config/server.toml + config/server.local.toml (port 15001); the client uses
    config/client.toml + config/client.local.toml. When the client exits the server is stopped,
    unless -KeepServer is given (or the server was already running before this script started).
    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER Config
    Debug (default) or Release build.

.PARAMETER KeepServer
    Leave the server running after the client exits.

.PARAMETER ClientArgs
    Extra arguments for vo_client.exe as one string (works with powershell -File), e.g.
    -ClientArgs '--size 1600x900 --ui-scale 1.25'.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\dev\run-dev.ps1
    powershell -ExecutionPolicy Bypass -File tools\dev\run-dev.ps1 -Config Release -KeepServer
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$KeepServer,
    [string]$ClientArgs = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Bin = Join-Path $Root ("build\x64-" + $Config.ToLowerInvariant() + "\bin")
$ServerExe = Join-Path $Bin 'vo_server.exe'
$ClientExe = Join-Path $Bin 'vo_client.exe'
$Port = 15001

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }

function Test-Port([int]$P) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $task = $client.ConnectAsync('127.0.0.1', $P)
        return ($task.Wait(300) -and $client.Connected)
    }
    catch { return $false }
    finally { $client.Dispose() }
}

foreach ($exe in @($ServerExe, $ClientExe)) {
    if (-not (Test-Path $exe)) { throw "$exe not found; build first: powershell -File build.ps1 -Config $Config" }
}

if (-not (Test-Path (Join-Path $Root 'config\keys\server_ed25519.pub')) -or
    -not (Test-Path (Join-Path $Root 'config\client.local.toml'))) {
    Write-Step 'Local dev setup missing - running tools/dev/setup-dev.ps1'
    & (Join-Path $PSScriptRoot 'setup-dev.ps1') -Config $Config
}

$server = $null
if (Test-Port $Port) {
    Write-Step "A server is already listening on 127.0.0.1:$Port - using it"
}
else {
    Write-Step "Starting vo_server in a new console ($ServerExe)"
    $server = Start-Process -FilePath $ServerExe -WorkingDirectory $Root -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while (-not (Test-Port $Port)) {
        if ($server.HasExited) { throw "vo_server exited with code $($server.ExitCode); see its console/log" }
        if ((Get-Date) -gt $deadline) { throw "vo_server is not listening on port $Port after 15 s" }
        Start-Sleep -Milliseconds 200
    }
    Write-Step "Server pid $($server.Id) listening on 127.0.0.1:$Port"
}

try {
    Write-Step 'Starting vo_client'
    $clientParams = @{ FilePath = $ClientExe; WorkingDirectory = $Root; PassThru = $true; Wait = $true }
    if ($ClientArgs.Trim()) { $clientParams.ArgumentList = $ClientArgs }
    $client = Start-Process @clientParams
    Write-Step "Client exited with code $($client.ExitCode)"
}
finally {
    if ($null -ne $server -and -not $KeepServer -and -not $server.HasExited) {
        Write-Step 'Stopping the server'
        Stop-Process -Id $server.Id -ErrorAction SilentlyContinue
        $server.WaitForExit(5000) | Out-Null
    }
}
