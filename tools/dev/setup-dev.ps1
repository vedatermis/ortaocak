<#
.SYNOPSIS
    One-time local development setup for VedatOnline (needs a built tree).

.DESCRIPTION
    1. Generates the server's Ed25519 key pair (config/keys/, git-ignored) with
       `vo_server --gen-keys` unless it already exists.
    2. Writes config/server.local.toml (git-ignored) with auth.dev_auto_register = true and a
       127.0.0.1 listen address, unless the file already exists.
    3. Writes the server public key into config/client.local.toml (git-ignored overlay of
       config/client.toml), unless the file already exists.
    Existing files are never overwritten without -Force (with -Force the keys are regenerated too,
    which invalidates every client that pinned the old key).
    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER Config
    Which build to use for vo_server.exe: Debug (default) or Release (falls back to the other one).

.PARAMETER Force
    Regenerate the keys and rewrite both local config files.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\dev\setup-dev.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$Force
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ConfigDir = Join-Path $Root 'config'
$KeyDir = Join-Path $ConfigDir 'keys'
$PubFile = Join-Path $KeyDir 'server_ed25519.pub'
$ServerLocal = Join-Path $ConfigDir 'server.local.toml'
$ClientLocal = Join-Path $ConfigDir 'client.local.toml'

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Note([string]$Message) { Write-Host "    $Message" }

function Find-ServerExe {
    $order = if ($Config -eq 'Debug') { @('x64-debug', 'x64-release') } else { @('x64-release', 'x64-debug') }
    foreach ($preset in $order) {
        $exe = Join-Path $Root "build\$preset\bin\vo_server.exe"
        if (Test-Path $exe) { return $exe }
    }
    throw "vo_server.exe not found under build\x64-debug or build\x64-release; run build.ps1 first"
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Read-PublicKey {
    $pub = ([IO.File]::ReadAllText($PubFile)).Trim()
    if ($pub -notmatch '^[0-9a-f]{64}$') { throw "$PubFile does not contain a 64-character hex key" }
    return $pub
}

$serverExe = Find-ServerExe
Write-Step "Using $serverExe"

# ---- 1. Server key pair ----
if ((Test-Path $PubFile) -and -not $Force) {
    Write-Step 'Server key pair already exists (config/keys) - kept'
}
else {
    Write-Step 'Generating the server key pair (vo_server --gen-keys)'
    $genArgs = @('--config', (Join-Path $ConfigDir 'server.toml'), '--gen-keys')
    if ($Force) { $genArgs += '--force' }
    # Native stderr must not become a terminating PowerShell error (5.1), so capture via files.
    $out = [IO.Path]::GetTempFileName()
    $err = [IO.Path]::GetTempFileName()
    $argLine = ($genArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $serverExe -ArgumentList $argLine -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError $err
    $output = [IO.File]::ReadAllText($out) + [IO.File]::ReadAllText($err)
    Remove-Item -Force $out, $err -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "vo_server --gen-keys failed (exit $($p.ExitCode)):`n$output" }
}
$publicKey = Read-PublicKey
Write-Note "server public key: $publicKey"

# ---- 2. server.local.toml ----
if ((Test-Path $ServerLocal) -and -not $Force) {
    Write-Step 'config/server.local.toml already exists - kept'
    $text = [IO.File]::ReadAllText($ServerLocal)
    if ($text -notmatch '(?m)^\s*dev_auto_register\s*=\s*true') {
        Write-Note 'note: it does not set [auth] dev_auto_register = true; new usernames will not be registered'
    }
}
else {
    Write-Step 'Writing config/server.local.toml'
    Write-Utf8NoBom $ServerLocal @"
# Local development overrides (written by tools/dev/setup-dev.ps1; git-ignored).
[net]
listen_address = "127.0.0.1"   # local only (no firewall prompt)

[auth]
dev_auto_register = true       # unknown username + valid password creates the account

[log]
level = "info"
"@
}

# ---- 3. client.local.toml ----
if ((Test-Path $ClientLocal) -and -not $Force) {
    Write-Step 'config/client.local.toml already exists - kept'
    $text = [IO.File]::ReadAllText($ClientLocal)
    if ($text -notmatch [regex]::Escape($publicKey)) {
        Write-Note 'warning: it does not contain the current server public key; the client will report'
        Write-Note "'Sunucu dogrulanamadi'. Re-run with -Force or set server_public_key = `"$publicKey`""
    }
}
else {
    Write-Step 'Writing config/client.local.toml'
    Write-Utf8NoBom $ClientLocal @"
# Local development overrides (written by tools/dev/setup-dev.ps1; git-ignored).
# Public key of the local dev server (config/keys/server_ed25519.pub).
server_public_key = "$publicKey"
"@
}

Write-Step 'Done. Start everything with: powershell -ExecutionPolicy Bypass -File tools\dev\run-dev.ps1'
