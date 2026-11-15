<#
.SYNOPSIS
    Runs the client UI automation flows (client/automation/flows/*.voscript) against real, isolated
    servers and collects screenshots and logs.

.DESCRIPTION
    For every flow: starts vo_server on a free 127.0.0.1 port with a temporary config and key pair
    (never the repository's config/ or port 15001; dev_auto_register on), writes a temporary
    client.toml pinning that key, runs vo_client with the flow's script(s), and stops the server
    again. Some flows use a special setup instead: no server (server down), a TCP listener that never
    answers (handshake timeout), a wrong pinned key, maintenance mode, a full server (max_connections
    = 1 held by botclient), a wrong protocol version, a duplicate login by botclient (HOOK kick) or a
    server killed mid-session (HOOK kill). Hooks are triggered by "log HOOK <name>" lines that the
    script writes to the client log.

    Output: <OutDir>/<flow>/<run>/*.png + client.log, <OutDir>/<flow>/server.*.log, summary.txt.
    A run fails if the client exits non-zero (script assertion, screenshot or runtime error), times
    out, or its log reports D3D debug-layer messages or live objects. The script fails if any run
    failed or a server process it started is still alive at the end.
    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER Config
    Debug (default) or Release build.

.PARAMETER OutDir
    Output directory (default: build/ui-flows/<config>-<timestamp>).

.PARAMETER Only
    Run only these flows (names as in the summary), e.g. -Only happy,delete.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\dev\run-ui-flows.ps1
    powershell -ExecutionPolicy Bypass -File tools\dev\run-ui-flows.ps1 -Only layout -OutDir $env:TEMP\vo_layout
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string]$OutDir = '',
    [string[]]$Only = @(),
    [int]$ClientTimeoutSec = 240
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# `powershell -File ... -Only a,b` passes one string: accept comma-separated names as well.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Bin = Join-Path $Root ("build\x64-" + $Config.ToLowerInvariant() + "\bin")
$ServerExe = Join-Path $Bin 'vo_server.exe'
$ClientExe = Join-Path $Bin 'vo_client.exe'
$BotExe = Join-Path $Bin 'botclient.exe'
$FlowsDir = Join-Path $Root 'client\automation\flows'
if (-not $OutDir) {
    $OutDir = Join-Path $Root ("build\ui-flows\" + $Config.ToLowerInvariant() + "-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
$Work = Join-Path $OutDir '_work'
$Utf8 = New-Object System.Text.UTF8Encoding($false)

$script:StartedPids = New-Object System.Collections.ArrayList
$script:Results = New-Object System.Collections.ArrayList

function Write-Info([string]$Message) { Write-Host "[ui-flows] $Message" }

function Quote([string]$Arg) {
    if ($Arg -match '[\s"]') { return '"' + ($Arg -replace '"', '\"') + '"' }
    return $Arg
}

function Read-Shared([string]$Path) {
    if (-not (Test-Path $Path)) { return '' }
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $reader = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
        return $reader.ReadToEnd()
    }
    finally { $fs.Dispose() }
}

# Runs an executable to completion; returns @{ Code; Out }.
function Invoke-Exe([string]$Exe, [string[]]$Arguments, [string]$LogBase, [int]$TimeoutSec = 60) {
    $argLine = ($Arguments | ForEach-Object { Quote $_ }) -join ' '
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -NoNewWindow -PassThru `
        -RedirectStandardOutput "$LogBase.out.log" -RedirectStandardError "$LogBase.err.log"
    $null = $p.Handle
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        throw "$(Split-Path -Leaf $Exe) $argLine did not finish within $TimeoutSec s"
    }
    $p.WaitForExit()
    return @{ Code = $p.ExitCode; Out = (Read-Shared "$LogBase.out.log") + (Read-Shared "$LogBase.err.log") }
}

function New-KeyPair([string]$Dir) {
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $cfg = Join-Path $Dir 'server.toml'
    [IO.File]::WriteAllText($cfg, "[log]`nlevel = `"warn`"`n", $Utf8)
    $r = Invoke-Exe $ServerExe @('--config', $cfg, '--gen-keys') (Join-Path $Dir 'genkeys')
    if ($r.Code -ne 0) { throw "vo_server --gen-keys failed ($($r.Code)): $($r.Out)" }
    $pub = ([IO.File]::ReadAllText((Join-Path $Dir 'keys\server_ed25519.pub'))).Trim()
    if ($pub -notmatch '^[0-9a-f]{64}$') { throw "malformed public key in $Dir" }
    return $pub
}

function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $l.Start()
    $port = $l.LocalEndpoint.Port
    $l.Stop()
    return $port
}

function Start-TestServer([string]$Dir, [hashtable]$Opts) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Dir 'keys') | Out-Null
    Copy-Item (Join-Path $Work 'keys_main\keys\*') (Join-Path $Dir 'keys') -Force
    $maintenance = if ($Opts.ContainsKey('Maintenance') -and $Opts.Maintenance) { 'true' } else { 'false' }
    $maxConn = if ($Opts.ContainsKey('MaxConnections')) { $Opts.MaxConnections } else { 50 }
    $cfg = Join-Path $Dir 'server.toml'
    [IO.File]::WriteAllText($cfg, @"
# Generated by tools/dev/run-ui-flows.ps1
[server]
name = "VedatOnline UI flows"
maintenance = $maintenance

[net]
listen_address = "127.0.0.1"
listen_port = 0
max_connections = $maxConn

[auth]
dev_auto_register = true

[world]
tick_rate = 20

[log]
level = "debug"
file = ""

[db]
connection_string = ""
"@, $Utf8)
    $portFile = Join-Path $Dir 'port.txt'
    if (Test-Path $portFile) { Remove-Item -Force $portFile }
    # --run-seconds is a dead-man switch in case this script dies.
    $argLine = "--config $(Quote $cfg) --port 0 --port-file $(Quote $portFile) --run-seconds 900"
    $p = Start-Process -FilePath $ServerExe -ArgumentList $argLine -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $Dir 'server.out.log') -RedirectStandardError (Join-Path $Dir 'server.err.log')
    $null = $p.Handle
    [void]$script:StartedPids.Add($p) # the Process object: a PID alone may be reused by another program
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) { throw "server exited early with code $($p.ExitCode) (see $Dir)" }
        if (Test-Path $portFile) {
            $text = ''
            try { $text = ([IO.File]::ReadAllText($portFile)).Trim() } catch { $text = '' }
            if ($text -match '^\d+$' -and [int]$text -gt 0) {
                Write-Info "  server pid $($p.Id) on 127.0.0.1:$text"
                return @{ Process = $p; Port = [int]$text; Dir = $Dir }
            }
        }
        Start-Sleep -Milliseconds 50
    }
    throw 'server did not report its port within 20 s'
}

function Stop-TestServer($Server) {
    if ($null -eq $Server) { return }
    $p = $Server.Process
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(10000) | Out-Null
    }
}

function Write-ClientConfig([string]$Path, [int]$Port, [string]$Key, [int]$ConnectTimeoutMs) {
    [IO.File]::WriteAllText($Path, @"
# Generated by tools/dev/run-ui-flows.ps1
server_public_key = "$Key"

[server]
host = "127.0.0.1"
port = $Port
connect_timeout_ms = $ConnectTimeoutMs
request_timeout_ms = 15000

[window]
width = 1280
height = 720
vsync = true

[log]
level = "info"
file = ""
"@, $Utf8)
}

# Checks a finished client log; returns a list of problems.
function Test-ClientLog([string]$Log, [bool]$ExpectScriptEnd) {
    $problems = @()
    if ($Log -match 'script failed: ([^\r\n]+)') { $problems += "script failed: $($Matches[1])" }
    if ($ExpectScriptEnd -and $Log -notmatch 'script finished') { $problems += 'script did not finish' }
    if ($Log -match 'D3D11 debug layer produced') { $problems += 'D3D11 debug layer messages' }
    if ($Log -match 'live-object report') { $problems += 'D3D11 live objects at shutdown' }
    if ($Log -match 'debug layer on' -and $Log -notmatch 'no live objects reported') {
        $problems += 'no live-object report although the debug layer was on'
    }
    if ($Log -match '\[(ERROR|FATAL)\]\s+\[T\d+\]\s+([^\r\n]+)') { $problems += "log error: $($Matches[2])" }
    return $problems
}

function Invoke-ClientRun([hashtable]$Flow, [hashtable]$Run, [string]$FlowDir, $Server, [int]$Port, [string]$Key) {
    $tag = $Run.Tag
    $runDir = Join-Path $FlowDir $tag
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $cfg = Join-Path $runDir 'client.toml'
    $connectMs = if ($Run.ContainsKey('ConnectTimeoutMs')) { $Run.ConnectTimeoutMs } else { 10000 }
    Write-ClientConfig $cfg $Port $Key $connectMs
    $script = if ($Run.Script -like '*.voscript') { Join-Path $Root $Run.Script } else { Join-Path $FlowsDir ($Run.Script + '.voscript') }
    $log = Join-Path $runDir 'client.log'
    $size = if ($Run.ContainsKey('Size')) { $Run.Size } else { '1280x720' }
    $clientArgs = @('--config', $cfg, '--script', $script, '--screenshot-dir', $runDir, '--log', $log,
        '--settings', (Join-Path $FlowDir 'settings.toml'), '--size', $size)
    if ($Run.ContainsKey('UiScale')) { $clientArgs += @('--ui-scale', $Run.UiScale) }
    if ($Run.ContainsKey('Scene')) { $clientArgs += @('--scene', $Run.Scene) }
    if ($Run.ContainsKey('NoArt') -and $Run.NoArt) { $clientArgs += '--no-art' }
    if ($Run.ContainsKey('Args')) { $clientArgs += $Run.Args }
    $argLine = ($clientArgs | ForEach-Object { Quote $_ }) -join ' '

    Write-Info "  run $($Flow.Name)/$tag ($($Run.Script), $size$(if ($Run.ContainsKey('UiScale')) { ' @' + $Run.UiScale }))"
    $started = Get-Date
    $p = Start-Process -FilePath $ClientExe -ArgumentList $argLine -WorkingDirectory $Root -PassThru
    $null = $p.Handle
    $hook = if ($Run.ContainsKey('Hook')) { $Run.Hook } else { '' }
    $hookDone = $false
    $problems = @()
    while (-not $p.WaitForExit(200)) {
        if ($hook -and -not $hookDone -and (Read-Shared $log) -match "script: HOOK $hook") {
            $hookDone = $true
            if ($hook -eq 'kill') {
                Write-Info "    HOOK kill: killing server pid $($Server.Process.Id)"
                Stop-Process -Id $Server.Process.Id -Force -ErrorAction SilentlyContinue
                $Server.Process.WaitForExit(10000) | Out-Null
            }
            elseif ($hook -eq 'kick') {
                Write-Info "    HOOK kick: botclient logs in as $($Run.HookUser)"
                $r = Invoke-Exe $BotExe @('--host', '127.0.0.1', '--port', "$Port", '--server-key', $Key, '--scenario', 'login',
                    '--user', $Run.HookUser, '--password', $Run.HookPassword, '--expect', 'ok') (Join-Path $runDir 'bot_kick') 60
                if ($r.Code -ne 0) { $problems += "kick bot failed: $($r.Out.Trim())" }
            }
        }
        if (((Get-Date) - $started).TotalSeconds -gt $ClientTimeoutSec) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $problems += "client did not finish within $ClientTimeoutSec s"
            break
        }
    }
    $p.WaitForExit()
    $code = $p.ExitCode
    if ($hook -and -not $hookDone) { $problems += "hook '$hook' was never requested" }
    if ($code -ne 0) { $problems += "client exit code $code" }
    $problems += Test-ClientLog (Read-Shared $log) $true
    $shots = @(Get-ChildItem -Path $runDir -Filter '*.png' -ErrorAction SilentlyContinue).Count
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    $ok = $problems.Count -eq 0
    [void]$script:Results.Add([pscustomobject]@{
            Flow = $Flow.Name; Run = $tag; Ok = $ok; ExitCode = $code; Screenshots = $shots; Seconds = $seconds
            Problems = ($problems -join '; ')
        })
    if ($ok) { Write-Info "    PASS ($shots screenshots, $seconds s)" }
    else { Write-Host "[ui-flows]     FAIL: $($problems -join '; ')" -ForegroundColor Red }
}

# Two clients at the same time against one server (flow "two_clients"): each side has its own
# script, settings file, log and screenshot directory. Both must exit 0; a side that is still running
# when the other failed is stopped.
function Invoke-TwoClientRun([hashtable]$Flow, [string]$FlowDir, [int]$Port, [string]$Key) {
    $sides = @()
    foreach ($side in $Flow.Sides) {
        $runDir = Join-Path $FlowDir $side.Tag
        New-Item -ItemType Directory -Force -Path $runDir | Out-Null
        $cfg = Join-Path $runDir 'client.toml'
        Write-ClientConfig $cfg $Port $Key 10000
        $log = Join-Path $runDir 'client.log'
        $size = if ($side.ContainsKey('Size')) { $side.Size } else { '1280x720' }
        $clientArgs = @('--config', $cfg, '--script', (Join-Path $FlowsDir ($side.Script + '.voscript')),
            '--screenshot-dir', $runDir, '--log', $log, '--settings', (Join-Path $runDir 'settings.toml'), '--size', $size)
        if ($side.ContainsKey('Args')) { $clientArgs += $side.Args }
        $argLine = ($clientArgs | ForEach-Object { Quote $_ }) -join ' '
        Write-Info "  run $($Flow.Name)/$($side.Tag) ($($side.Script), $size)"
        $p = Start-Process -FilePath $ClientExe -ArgumentList $argLine -WorkingDirectory $Root -PassThru
        $null = $p.Handle
        $sides += @{ Tag = $side.Tag; Process = $p; Log = $log; Dir = $runDir; Problems = @(); Done = $false }
    }
    $started = Get-Date
    while ($true) {
        $running = 0
        foreach ($s in $sides) {
            if ($s.Done) { continue }
            if ($s.Process.HasExited) {
                $s.Process.WaitForExit()
                $s.Done = $true
                if ($s.Process.ExitCode -ne 0) { $s.Problems += "client exit code $($s.Process.ExitCode)" }
            }
            else { $running++ }
        }
        if ($running -eq 0) { break }
        $anyFailed = @($sides | Where-Object { $_.Done -and $_.Problems.Count -gt 0 }).Count -gt 0
        $timedOut = ((Get-Date) - $started).TotalSeconds -gt $ClientTimeoutSec
        if ($anyFailed -or $timedOut) {
            foreach ($s in $sides) {
                if (-not $s.Done) {
                    Stop-Process -Id $s.Process.Id -Force -ErrorAction SilentlyContinue
                    $s.Process.WaitForExit(10000) | Out-Null
                    $s.Done = $true
                    $s.Problems += $(if ($timedOut) { "client did not finish within $ClientTimeoutSec s" } else { 'stopped: the other client failed' })
                }
            }
            break
        }
        Start-Sleep -Milliseconds 200
    }
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    foreach ($s in $sides) {
        $problems = @($s.Problems) + @(Test-ClientLog (Read-Shared $s.Log) $true)
        $shots = @(Get-ChildItem -Path $s.Dir -Filter '*.png' -ErrorAction SilentlyContinue).Count
        $ok = $problems.Count -eq 0
        [void]$script:Results.Add([pscustomobject]@{
                Flow = $Flow.Name; Run = $s.Tag; Ok = $ok; ExitCode = $s.Process.ExitCode; Screenshots = $shots; Seconds = $seconds
                Problems = ($problems -join '; ')
            })
        if ($ok) { Write-Info "    $($s.Tag): PASS ($shots screenshots, $seconds s)" }
        else { Write-Host "[ui-flows]     $($s.Tag): FAIL: $($problems -join '; ')" -ForegroundColor Red }
    }
}
# ---- Flow table ----
$hookPassword = 'gizli123'
$Flows = @(
    @{ Name = 'happy'; Runs = @(@{ Tag = '1_happy'; Script = 'happy_path' }, @{ Tag = '2_remember'; Script = 'remember_check' }) },
    @{ Name = 'happy_noart'; Runs = @(@{ Tag = 'run'; Script = 'happy_path'; NoArt = $true }) },
    @{ Name = 'wrong_password'; Runs = @(@{ Tag = 'run'; Script = 'wrong_password' }) },
    @{ Name = 'validation'; Runs = @(@{ Tag = 'run'; Script = 'validation' }) },
    @{ Name = 'name_taken'; Runs = @(@{ Tag = 'run'; Script = 'name_taken' }) },
    @{ Name = 'delete'; Runs = @(@{ Tag = 'run'; Script = 'delete' }) },
    @{ Name = 'server_down'; Server = 'none'; Runs = @(@{ Tag = 'run'; Script = 'server_down' }) },
    @{ Name = 'timeout_cancel'; Server = 'blackhole'; Runs = @(@{ Tag = 'run'; Script = 'timeout_cancel'; ConnectTimeoutMs = 3000 }) },
    @{ Name = 'wrong_key'; ClientKey = 'other'; Runs = @(@{ Tag = 'run'; Script = 'wrong_key' }) },
    @{ Name = 'maintenance'; ServerOpts = @{ Maintenance = $true }; Runs = @(@{ Tag = 'run'; Script = 'maintenance' }) },
    @{ Name = 'server_full'; ServerOpts = @{ MaxConnections = 1 }; HoldBot = $true; Runs = @(@{ Tag = 'run'; Script = 'server_full' }) },
    @{ Name = 'version_mismatch'; Runs = @(@{ Tag = 'run'; Script = 'version_mismatch'; Args = @('--protocol-version', '99') }) },
    @{ Name = 'too_many_attempts'; Runs = @(@{ Tag = 'run'; Script = 'too_many_attempts' }) },
    @{ Name = 'kicked'; Runs = @(@{ Tag = 'run'; Script = 'kicked'; Hook = 'kick'; HookUser = 'm4_kick'; HookPassword = $hookPassword }) },
    @{ Name = 'server_killed'; Runs = @(@{ Tag = 'run'; Script = 'server_killed'; Hook = 'kill' }) },
    @{ Name = 'layout'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1024x640'; Script = 'layout'; Size = '1024x640' },
            @{ Tag = '1920x1080'; Script = 'layout'; Size = '1920x1080' },
            @{ Tag = '1920x1080_150'; Script = 'layout'; Size = '1920x1080'; UiScale = '1.5' },
            @{ Tag = '1024x640_150'; Script = 'layout'; Size = '1024x640'; UiScale = '1.5' },
            @{ Tag = '1024x640_noart'; Script = 'layout'; Size = '1024x640'; NoArt = $true },
            @{ Tag = '1920x1080_150_noart'; Script = 'layout'; Size = '1920x1080'; UiScale = '1.5'; NoArt = $true })
    },
    @{ Name = 'uidemo'; Server = 'none'; Runs = @(@{ Tag = 'run'; Script = 'client\automation\uidemo.voscript'; Scene = 'uidemo'; Size = '1280x720' }) },
    # ---- M6 world flows (need assets/; built below if missing). --fixed-dt makes movement per frame
    # deterministic; vsync stays on so MOVE_REQ timing is real-time-ish against a real server. ----
    @{ Name = 'world_tour'; Runs = @(@{ Tag = 'run'; Script = 'world_tour'; Size = '1600x900'; Args = @('--fixed-dt', '0.0333') }) },
    @{ Name = 'world_ghosts'; Runs = @(@{ Tag = 'run'; Script = 'world_ghosts'; Size = '1600x900'; Args = @('--fixed-dt', '0.0333', '--debug-ghosts', '8') }) },
    @{ Name = 'world_preview'; Runs = @(@{ Tag = 'run'; Script = 'world_preview'; Size = '1280x720' }) },
    # Logs FPS / draw calls / triangles (no thresholds: numbers depend on the machine).
    @{ Name = 'world_perf'; Runs = @(@{ Tag = 'run'; Script = 'world_perf'; Size = '1920x1080'; Args = @('--vsync', 'off', '--debug-ghosts', '8') }) },
    # ---- M7: two real clients in one world (Aydor walks to and around a Kargan, then leaves). ----
    # Resize while loading, minimise/restore, Alt+Tab, simulated device loss (client resilience).
    @{ Name = 'resilience'; Runs = @(@{ Tag = 'run'; Script = 'world_resilience'; Size = '1280x720' }) },
    # The same view without and with 4x MSAA (graphics.msaa); a fresh server per run, so the script is the same.
    @{ Name = 'msaa'; ServerPerRun = $true; Runs = @(@{ Tag = 'msaa1'; Script = 'world_msaa'; Size = '1280x720'; Args = @('--msaa', '1', '--fixed-dt', '0.0333') }, @{ Tag = 'msaa4'; Script = 'world_msaa'; Size = '1280x720'; Args = @('--msaa', '4', '--fixed-dt', '0.0333') }) },
    @{ Name = 'two_clients'; Two = $true; Sides = @(@{ Tag = 'a_aydor'; Script = 'two_aydor'; Size = '1280x720' }, @{ Tag = 'b_kargan'; Script = 'two_kargan'; Size = '1280x720' }); Runs = @() }
)

# ---- Main ----
$exitCode = 1
try {
    foreach ($exe in @($ServerExe, $ClientExe, $BotExe)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
    # The world flows and the 3D preview need the runtime assets (git-ignored): build them if missing.
    if (-not (Test-Path (Join-Path $Root 'assets\terrain\zone1\zone1.vterrain'))) {
        $assetc = Join-Path $Bin 'assetc.exe'
        if (-not (Test-Path $assetc)) { throw "missing $assetc (build first)" }
        Write-Info 'assets/ missing: assetc build assets_src assets'
        & $assetc build (Join-Path $Root 'assets_src') (Join-Path $Root 'assets') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "assetc build failed ($LASTEXITCODE)" }
    }
    if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    Write-Info "output: $OutDir"
    $mainKey = New-KeyPair (Join-Path $Work 'keys_main')
    $otherKey = New-KeyPair (Join-Path $Work 'keys_other')

    foreach ($flow in $Flows) {
        if ($Only.Count -gt 0 -and $Only -notcontains $flow.Name) { continue }
        Write-Info "flow $($flow.Name)"
        $flowDir = Join-Path $OutDir $flow.Name
        if (Test-Path $flowDir) { Remove-Item -Recurse -Force $flowDir }
        New-Item -ItemType Directory -Force -Path $flowDir | Out-Null
        $mode = if ($flow.ContainsKey('Server')) { $flow.Server } else { 'normal' }
        $serverOpts = if ($flow.ContainsKey('ServerOpts')) { $flow.ServerOpts } else { @{} }
        $key = if ($flow.ContainsKey('ClientKey') -and $flow.ClientKey -eq 'other') { $otherKey } else { $mainKey }
        $perRun = $flow.ContainsKey('ServerPerRun') -and $flow.ServerPerRun
        $server = $null
        $listener = $null
        $bot = $null
        $port = 0
        try {
            if ($mode -eq 'none') { $port = Get-FreePort }
            elseif ($mode -eq 'blackhole') {
                # Accepts TCP connections (kernel backlog) but never reads or answers.
                $listener = New-Object System.Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
                $listener.Start()
                $port = $listener.LocalEndpoint.Port
                Write-Info "  silent listener on 127.0.0.1:$port"
            }
            elseif (-not $perRun) {
                $server = Start-TestServer (Join-Path $flowDir 'server') $serverOpts
                $port = $server.Port
            }
            if ($flow.ContainsKey('HoldBot') -and $flow.HoldBot) {
                $botBase = Join-Path $flowDir 'bot_hold'
                $botArgs = "--host 127.0.0.1 --port $port --server-key $key --scenario waitshutdown --user m4_holder --timeout-ms 600000"
                $bot = Start-Process -FilePath $BotExe -ArgumentList $botArgs -NoNewWindow -PassThru `
                    -RedirectStandardOutput "$botBase.out.log" -RedirectStandardError "$botBase.err.log"
                $null = $bot.Handle
                $deadline = (Get-Date).AddSeconds(15)
                while ((Read-Shared "$botBase.out.log") -notmatch 'logged in, waiting') {
                    if ($bot.HasExited -or (Get-Date) -gt $deadline) { throw 'the holding bot did not log in' }
                    Start-Sleep -Milliseconds 100
                }
                Write-Info '  bot holds the only connection slot'
            }
            if ($flow.ContainsKey('Two') -and $flow.Two) { Invoke-TwoClientRun $flow $flowDir $port $key }
            foreach ($run in $flow.Runs) {
                $runServer = $server
                if ($perRun) {
                    $runServer = Start-TestServer (Join-Path $flowDir ($run.Tag + '_server')) $serverOpts
                    $port = $runServer.Port
                }
                try { Invoke-ClientRun $flow $run $flowDir $runServer $port $key }
                finally { if ($perRun) { Stop-TestServer $runServer } }
            }
        }
        catch {
            Write-Host "[ui-flows]   flow error: $($_.Exception.Message)" -ForegroundColor Red
            [void]$script:Results.Add([pscustomobject]@{
                    Flow = $flow.Name; Run = '-'; Ok = $false; ExitCode = -1; Screenshots = 0; Seconds = 0
                    Problems = $_.Exception.Message
                })
        }
        finally {
            if ($null -ne $bot -and -not $bot.HasExited) { Stop-Process -Id $bot.Id -Force -ErrorAction SilentlyContinue }
            Stop-TestServer $server
            if ($null -ne $listener) { $listener.Stop() }
            if ($null -ne $server) {
                foreach ($f in @('server.out.log', 'server.err.log')) {
                    $src = Join-Path $server.Dir $f
                    if (Test-Path $src) { Copy-Item $src (Join-Path $flowDir $f) -Force }
                }
            }
        }
    }
}
finally {
    # Every server this script started must be gone.
    $leaked = @()
    foreach ($proc in $script:StartedPids) {
        if (-not $proc.HasExited) {
            $leaked += $proc.Id
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($null -ne $Error -and $Error.Count -gt 0 -and $script:Results.Count -eq 0) {
        Write-Host "[ui-flows] setup error: $($Error[0].Exception.Message)" -ForegroundColor Red
    }
    if ($leaked.Count -gt 0) {
        [void]$script:Results.Add([pscustomobject]@{
                Flow = '(cleanup)'; Run = '-'; Ok = $false; ExitCode = -1; Screenshots = 0; Seconds = 0
                Problems = "leaked server process(es): $($leaked -join ', ')"
            })
    }
    $table = $script:Results | Format-Table Flow, Run, Ok, ExitCode, Screenshots, Seconds, Problems -AutoSize -Wrap | Out-String -Width 220
    if (Test-Path $OutDir) { [IO.File]::WriteAllText((Join-Path $OutDir 'summary.txt'), $table, $Utf8) }
    Write-Host $table
    $failed = @($script:Results | Where-Object { -not $_.Ok }).Count
    $total = $script:Results.Count
    Write-Info "$($total - $failed)/$total runs passed; $($script:StartedPids.Count) server process(es) started, $($leaked.Count) leaked"
    if ($failed -eq 0 -and $total -gt 0) { $exitCode = 0 }
}
exit $exitCode
