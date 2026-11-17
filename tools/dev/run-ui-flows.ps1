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
$AdminExe = Join-Path $Bin 'vo_admin.exe'
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
    # A fixed world RNG seed (M11): monster placement, wandering and the damage / critical / dodge
    # rolls are the same in every run for the same inputs (real-time timing still varies a little).
    $seed = if ($Opts.ContainsKey('Seed')) { $Opts.Seed } else { 4711 }
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
seed = $seed

[log]
level = "debug"
file = ""

[db]
connection_string = ""
$(if ($Opts.ContainsKey('Schema') -and $Opts.Schema) { "schema = `"$($Opts.Schema)`"`nrequired = true`npool_size = 2" })
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

# ---- Test database (flows with Db = $true, M10) ----
# Flows that need data a client cannot create (vo_admin set-level / give-item) run their server on
# PostgreSQL instead of the in-memory backend: only database vedatonline_test, through the test
# configuration the unit tests use (VEDAT_TEST_DB_URL, else db.test_connection_string of the first
# config/server.local.toml found upwards from the repository root), in an own schema vo_it_<time>_<random>
# that vo_admin drops again afterwards (crashed runs are swept by the tests after two hours). The URL is
# never printed and never written to a file: vo_server and vo_admin inherit it as VEDAT_DB_URL from this
# process only. Without a test database such a flow is reported as skipped.
function Find-LocalServerToml {
    $dir = $Root
    while ($dir) {
        $candidate = Join-Path $dir 'config\server.local.toml'
        if (Test-Path $candidate) { return $candidate }
        $parent = Split-Path -Parent $dir
        if (-not $parent -or $parent -eq $dir) { break }
        $dir = $parent
    }
    return $null
}

function Get-LocalTomlValue([string]$Key) {
    $local = Find-LocalServerToml
    if (-not $local) { return $null }
    $m = Select-String -Path $local -Pattern ('^\s*' + [regex]::Escape($Key) + '\s*=\s*"([^"]*)"') | Select-Object -First 1
    if ($null -eq $m) { return $null }
    return $m.Matches[0].Groups[1].Value
}

# Returns the schema name, or $null when no test database is configured.
function Initialize-FlowDb {
    $url = $env:VEDAT_TEST_DB_URL
    if ($url -eq 'skip') { return $null }
    if (-not $url) { $url = Get-LocalTomlValue 'test_connection_string' }
    if (-not $url) { return $null }
    if ($url -notmatch '(/vedatonline_test(\?|$))|(dbname\s*=\s*vedatonline_test(\s|$))') {
        throw 'the test database URL must name database vedatonline_test (refusing to use it)'
    }
    if (-not (Test-Path $AdminExe)) { throw "missing $AdminExe (needed to seed and drop the test schema)" }
    $env:VEDAT_DB_URL = $url
    $unix = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
    return ('vo_it_{0}_{1:x8}' -f $unix, (Get-Random -Maximum 2147483647))
}

function Clear-FlowDb([string]$Schema, [string]$ConfigFile) {
    try {
        if ($Schema -and $ConfigFile -and (Test-Path $ConfigFile)) {
            $r = Invoke-Exe $AdminExe @('--config', $ConfigFile, 'testdb', 'drop-schema', $Schema) (Join-Path $Work 'drop_schema') 60
            if ($r.Code -ne 0) { Write-Host "[ui-flows]   warning: dropping $Schema failed (exit $($r.Code))" -ForegroundColor Yellow }
            else { Write-Info "  dropped test schema $Schema" }
        }
    }
    finally { Remove-Item Env:VEDAT_DB_URL -ErrorAction SilentlyContinue }
}

# vo_admin <args> against the run's server configuration (its schema); throws on a failure.
function Invoke-Admin([string]$ConfigFile, [string[]]$Arguments, [string]$LogBase) {
    $r = Invoke-Exe $AdminExe (@('--config', $ConfigFile) + $Arguments) $LogBase 60
    # (Its output only on success: a database error message may name the host or the user.)
    if ($r.Code -ne 0) { throw "vo_admin $($Arguments -join ' ') exited $($r.Code) (see $LogBase.*.log)" }
    Write-Info "    vo_admin $($Arguments -join ' '): $($r.Out.Trim())"
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
    # A run may ask for more time than -ClientTimeoutSec (the M9 hunting flow), never less.
    $runLimit = if ($Run.ContainsKey('TimeoutSec')) { [math]::Max($Run.TimeoutSec, $ClientTimeoutSec) } else { $ClientTimeoutSec }
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
        if (((Get-Date) - $started).TotalSeconds -gt $runLimit) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $problems += "client did not finish within $runLimit s"
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
    $flowLimit = if ($Flow.ContainsKey('TimeoutSec')) { [math]::Max($Flow.TimeoutSec, $ClientTimeoutSec) } else { $ClientTimeoutSec }
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
        $timedOut = ((Get-Date) - $started).TotalSeconds -gt $flowLimit
        if ($anyFailed -or $timedOut) {
            foreach ($s in $sides) {
                if (-not $s.Done) {
                    Stop-Process -Id $s.Process.Id -Force -ErrorAction SilentlyContinue
                    $s.Process.WaitForExit(10000) | Out-Null
                    $s.Done = $true
                    $s.Problems += $(if ($timedOut) { "client did not finish within $flowLimit s" } else { 'stopped: the other client failed' })
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
    @{ Name = 'two_clients'; Two = $true; Sides = @(@{ Tag = 'a_aydor'; Script = 'two_aydor'; Size = '1280x720' }, @{ Tag = 'b_kargan'; Script = 'two_kargan'; Size = '1280x720' }); Runs = @() },
    # ---- Polish round 1: character looks (preview / world mesh per race + look) and the story /
    # camera / walk-speed / jump+sit tour at three window sizes (a fresh server per run). ----
    @{ Name = 'looks'; Runs = @(@{ Tag = 'run'; Script = 'looks'; Size = '1280x720'; Args = @('--fixed-dt', '0.0333') }) },
    @{ Name = 'polish'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'polish'; Size = '1280x720'; Args = @('--fixed-dt', '0.0333') },
            @{ Tag = '1024x640'; Script = 'polish'; Size = '1024x640'; Args = @('--fixed-dt', '0.0333') },
            @{ Tag = '1024x640_150'; Script = 'polish'; Size = '1024x640'; UiScale = '1.5'; Args = @('--fixed-dt', '0.0333') })
    },
    # ---- M8 "Kuşan": inventory window, drag & drop, tooltips, persistence (three sizes / UI scales,
    # a fresh server per run), the class kits on the creation screen, weapons seen by another client. ----
    @{ Name = 'items'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'items'; Size = '1280x720'; Args = @('--fixed-dt', '0.0333') },
            @{ Tag = '1600x900'; Script = 'items'; Size = '1600x900'; Args = @('--fixed-dt', '0.0333') },
            @{ Tag = '1600x900_125'; Script = 'items'; Size = '1600x900'; UiScale = '1.25'; Args = @('--fixed-dt', '0.0333') })
    },
    @{ Name = 'classes'; Runs = @(@{ Tag = 'run'; Script = 'classes'; Size = '1280x720' }) },
    @{ Name = 'two_clients_items'; Two = $true; Sides = @(@{ Tag = 'a_warrior'; Script = 'two_items_a'; Size = '1280x720' }, @{ Tag = 'b_rogue'; Script = 'two_items_b'; Size = '1280x720' }); Runs = @() },
    # ---- M9 "İlk Kan": monsters, targeting, auto-attack + skill, damage numbers, death / respawn,
    # level-up, another client watching (real time: no --fixed-dt, server timers are real). ----
    @{ Name = 'combat'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'combat'; Size = '1280x720' },
            @{ Tag = '1600x900_125'; Script = 'combat'; Size = '1600x900'; UiScale = '1.25' })
    },
    @{ Name = 'combat_mage'; Runs = @(@{ Tag = 'run'; Script = 'combat_mage'; Size = '1280x720' }) },
    @{ Name = 'death'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'death'; Size = '1280x720' },
            @{ Tag = '1024x640_150'; Script = 'death'; Size = '1024x640'; UiScale = '1.5' })
    },
    @{ Name = 'levelup'; Runs = @(@{ Tag = '1920x1080'; Script = 'levelup'; Size = '1920x1080'; TimeoutSec = 330 }) },
    @{ Name = 'two_clients_combat'; Two = $true; TimeoutSec = 300; Sides = @(@{ Tag = 'a_warrior'; Script = 'two_combat_a'; Size = '1280x720' }, @{ Tag = 'b_priest'; Script = 'two_combat_b'; Size = '1280x720' }); Runs = @() },
    # ---- M10 "Ganimet": loot bags, the loot window, take-all, the coin balance, stacks; another client
    # sees the bag locked and cannot take from it (real time, a fresh server per run). ----
    @{ Name = 'loot'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'loot'; Size = '1280x720'; TimeoutSec = 600 },
            @{ Tag = '1024x640_150'; Script = 'loot'; Size = '1024x640'; UiScale = '1.5'; TimeoutSec = 600 })
    },
    # Needs the PostgreSQL test database (skipped without one): run 1 creates a warrior, vo_admin seeds it in
    # the flow's own test schema (level 10, 1234567 coins, a rare and an uncommon item), run 2 shows the
    # grouped balance and the rarity tooltips, then hunts until a bag with two or more rows is open.
    @{ Name = 'loot_rich'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'loot_rich_create'; Size = '1280x720' },
            @{ Tag = '2_rich'; Script = 'loot_rich'; Size = '1600x900'; TimeoutSec = 480; Admin = @(
                    @('set-level', 'Zenginci', '10'), @('set-coins', 'Zenginci', '1234567'),
                    @('give-item', 'Zenginci', '11', '3'), @('give-item', 'Zenginci', '15')) })
    },
    @{ Name = 'two_clients_loot'; Two = $true; TimeoutSec = 300; Sides = @(@{ Tag = 'a_owner'; Script = 'two_loot_a'; Size = '1280x720' }, @{ Tag = 'b_other'; Script = 'two_loot_b'; Size = '1280x720' }); Runs = @() },
    # ---- M11 "Künye": the character sheet - level 1 without points, hunt to level 2, pending points with
    # the preview, Vazgeç, Onayla, relog (three sizes / UI scales, a fresh server per run). ----
    @{ Name = 'stats'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'stats'; Size = '1280x720'; TimeoutSec = 420 },
            @{ Tag = '1024x640_150'; Script = 'stats'; Size = '1024x640'; UiScale = '1.5'; TimeoutSec = 420 },
            @{ Tag = '1920x1080_150'; Script = 'stats'; Size = '1920x1080'; UiScale = '1.5'; TimeoutSec = 420 })
    },
    # Needs the PostgreSQL test database (skipped without one): a level-10 warrior (vo_admin set-level) spends
    # its 27 points with Shift+click next to the open inventory, then equips a Bekçi Kalkanı (give-item):
    # the item bonus appears separately; run 3 shows the result in a larger window.
    @{ Name = 'stats_rich'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'stats_rich_create'; Size = '1280x720' },
            @{ Tag = '2_spend_1280x720'; Script = 'stats_rich'; Size = '1280x720'; Admin = @(
                    @('set-level', 'Kunyeli', '10'), @('give-item', 'Kunyeli', '15')) },
            @{ Tag = '3_view_1600x900'; Script = 'stats_rich_view'; Size = '1600x900' })
    },
    # Needs the PostgreSQL test database: a level-10 rogue with 40 Çeviklik (16 % dodge) lets a Kül Çömezi hit
    # it until a blow is dodged ("Iska").
    @{ Name = 'dodge'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'dodge_create'; Size = '1280x720' },
            @{ Tag = '2_dodge'; Script = 'dodge'; Size = '1280x720'; TimeoutSec = 420; Admin = @(, @('set-level', 'Kacak', '10')) })
    },
    # ---- M12 "Yetenek Yolları": the skill bar (default skill, tooltip, drag / swap / clear, use with the
    # cooldown sweep, stored across a relog), at 1280x720 and 1024x640 @150 % (a fresh server per run). ----
    @{ Name = 'skills_bar'; ServerPerRun = $true; Runs = @(
            @{ Tag = '1280x720'; Script = 'skills_bar'; Size = '1280x720'; TimeoutSec = 420 },
            @{ Tag = '1024x640_150'; Script = 'skills_bar'; Size = '1024x640'; UiScale = '1.5'; TimeoutSec = 420 })
    },
    # Needs the PostgreSQL test database (skipped without one): a level-10 warrior (vo_admin set-level) spends
    # its 9 skill points in the window (confirmation, tier gates), drags skills onto the bar, uses a buff
    # (icon, tooltip, aura), pulls two monsters, taunts them and hits both with Yarma; run 3 checks the
    # stored ranks and bar and the window at 1024x640 @150 %.
    @{ Name = 'skills_warrior'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'skills_warrior_create'; Size = '1280x720' },
            @{ Tag = '2_tree_1280x720'; Script = 'skills_warrior'; Size = '1280x720'; TimeoutSec = 480; Admin = @(, @('set-level', 'Yolcu', '10')) },
            @{ Tag = '3_view_1024x640_150'; Script = 'skills_warrior_view'; Size = '1024x640'; UiScale = '1.5' })
    },
    # One flow per other class (test database): a level-10 rogue leaps (Gölge Adımı), throws Sis Bombası (fog,
    # slow) and Zehirli Kesik (poison ticks on the target frame); a mage raises Buz Kabuğu (absorb shield,
    # "Emildi"), Kırağı Oku (frost bolt, slow), Yakıcı İz (burn ticks) and Kor Yağmuru (ember rain); a priest
    # puts Kutlu Siper and Yenileyen Işık on itself, Halka Şifası, Bağlayan Söz (weaken + slow) and Işık
    # Çarpması on the elite Kor Bekçisi.
    @{ Name = 'skills_rogue'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'skills_rogue_create'; Size = '1280x720' },
            @{ Tag = '2_rogue'; Script = 'skills_rogue'; Size = '1280x720'; TimeoutSec = 480; Admin = @(, @('set-level', 'Golgeci', '10')) })
    },
    @{ Name = 'skills_mage'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'skills_mage_create'; Size = '1280x720' },
            @{ Tag = '2_mage'; Script = 'skills_mage'; Size = '1280x720'; TimeoutSec = 480; Admin = @(, @('set-level', 'Kozcu', '10')) })
    },
    @{ Name = 'skills_priest'; Db = $true; Runs = @(
            @{ Tag = '1_create'; Script = 'skills_priest_create'; Size = '1280x720' },
            @{ Tag = '2_priest'; Script = 'skills_priest'; Size = '1280x720'; TimeoutSec = 480; Admin = @(, @('set-level', 'Isikci', '10')) })
    }
)

# ---- Main ----
$exitCode = 1
try {
    foreach ($exe in @($ServerExe, $ClientExe, $BotExe)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
    # The world flows and the 3D preview need the runtime assets (git-ignored): build them if missing.
    if (-not (Test-Path (Join-Path $Root 'assets\terrain\zone1\zone1.vterrain'))) {
        Write-Info 'assets/ missing: tools\build-assets.ps1 (assetc build assets_src assets, verified)'
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\build-assets.ps1') -Config $Config | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "tools\build-assets.ps1 failed ($LASTEXITCODE)" }
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
        $dbSchema = $null
        if ($flow.ContainsKey('Db') -and $flow.Db) {
            try { $dbSchema = Initialize-FlowDb }
            catch {
                Write-Host "[ui-flows]   flow error: $($_.Exception.Message)" -ForegroundColor Red
                [void]$script:Results.Add([pscustomobject]@{
                        Flow = $flow.Name; Run = '-'; Ok = $false; ExitCode = -1; Screenshots = 0; Seconds = 0
                        Problems = $_.Exception.Message
                    })
                continue
            }
            if (-not $dbSchema) {
                Write-Info '  SKIPPED: no test database (VEDAT_TEST_DB_URL or db.test_connection_string in config/server.local.toml)'
                [void]$script:Results.Add([pscustomobject]@{
                        Flow = $flow.Name; Run = '-'; Ok = $true; ExitCode = 0; Screenshots = 0; Seconds = 0
                        Problems = 'SKIPPED: no test database'
                    })
                continue
            }
            Write-Info "  PostgreSQL test schema $dbSchema in vedatonline_test"
            $serverOpts = $serverOpts.Clone()
            $serverOpts.Schema = $dbSchema
        }
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
                try {
                    # Seeding before the run (Db flows): vo_admin on the run's schema, e.g. set-level.
                    if ($run.ContainsKey('Admin')) {
                        foreach ($adminArgs in $run.Admin) {
                            Invoke-Admin (Join-Path $runServer.Dir 'server.toml') $adminArgs (Join-Path $flowDir ($run.Tag + '_admin'))
                        }
                    }
                    Invoke-ClientRun $flow $run $flowDir $runServer $port $key
                }
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
            if ($dbSchema) { Clear-FlowDb $dbSchema $(if ($null -ne $server) { Join-Path $server.Dir 'server.toml' } else { '' }) }
        }
    }
}
finally {
    Remove-Item Env:VEDAT_DB_URL -ErrorAction SilentlyContinue
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
