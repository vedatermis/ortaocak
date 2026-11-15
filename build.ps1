<#
.SYNOPSIS
    Configures, builds and (optionally) tests VedatOnline from a plain PowerShell prompt.

.DESCRIPTION
    Loads the Visual Studio x64 developer environment (vcvars64.bat), then drives CMake presets
    (Ninja, x64, vcpkg manifest mode). Nothing needs to be on PATH beforehand.
    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER Config
    Debug (default) or Release. Maps to the CMake presets x64-debug / x64-release.

.PARAMETER Target
    Build only this CMake target (e.g. vo_server, vo_client, vo_tests).

.PARAMETER Test
    Run ctest after building.

.PARAMETER Clean
    Delete build/<preset> before configuring (vcpkg packages in build/vcpkg_installed are kept).

.EXAMPLE
    powershell -File build.ps1
    powershell -File build.ps1 -Config Release -Test
    powershell -File build.ps1 -Target vo_client

.NOTES
    Environment overrides:
      VO_VS_PATH      Visual Studio installation path (default: vswhere, then known locations)
      VO_VCPKG_ROOT   vcpkg root (default: external/vcpkg if present, else the VS-bundled vcpkg)
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string]$Target = '',
    [switch]$Test,
    [switch]$Clean
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Root = $PSScriptRoot
$Preset = 'x64-' + $Config.ToLowerInvariant()
$BuildDir = Join-Path $Root (Join-Path 'build' $Preset)

function Write-Step([string]$Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Find-VisualStudio {
    if ($env:VO_VS_PATH) {
        if (Test-Path (Join-Path $env:VO_VS_PATH 'VC\Auxiliary\Build\vcvars64.bat')) { return $env:VO_VS_PATH }
        throw "VO_VS_PATH '$($env:VO_VS_PATH)' does not contain VC\Auxiliary\Build\vcvars64.bat"
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -prerelease -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $found) {
            $first = @($found)[0]
            if (Test-Path (Join-Path $first 'VC\Auxiliary\Build\vcvars64.bat')) { return $first }
        }
    }
    foreach ($version in @('18', '17')) {
        foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools')) {
            $candidate = Join-Path $env:ProgramFiles "Microsoft Visual Studio\$version\$edition"
            if (Test-Path (Join-Path $candidate 'VC\Auxiliary\Build\vcvars64.bat')) { return $candidate }
        }
    }
    throw 'Visual Studio with the C++ x64 toolset was not found (set VO_VS_PATH).'
}

function Import-VsDevEnvironment([string]$VsPath) {
    $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
    # vcvars uses vswhere internally; make sure it is reachable to avoid noisy errors.
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    if ((Test-Path $installer) -and ($env:PATH -notlike "*$installer*")) {
        $env:PATH = "$installer;$env:PATH"
    }
    $lines = & cmd.exe /c "call `"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed (exit $LASTEXITCODE)" }
    foreach ($line in $lines) {
        $idx = $line.IndexOf('=')
        if ($idx -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $idx), $line.Substring($idx + 1), 'Process')
        }
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'vcvars64.bat did not set up an x64 environment.' }
}

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    Write-Host "   $Exe $($Arguments -join ' ')" -ForegroundColor DarkGray
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # native stderr must not turn into terminating errors
    try {
        & $Exe @Arguments
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $saved
    }
    if ($code -ne 0) { throw "$Exe failed with exit code $code" }
}

try {
    $started = Get-Date

    # ---- Developer environment ----
    $haveCl = $null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue)
    if ($haveCl -and $env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
        Write-Step 'Using existing x64 developer environment'
        $vsPath = $env:VSINSTALLDIR
    }
    else {
        $vsPath = Find-VisualStudio
        Write-Step "Loading VS x64 developer environment from $vsPath"
        Import-VsDevEnvironment $vsPath
    }
    foreach ($tool in @('cl.exe', 'cmake.exe', 'ninja.exe')) {
        if ($null -eq (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "$tool not found after loading the developer environment."
        }
    }

    # ---- vcpkg ----
    $externalVcpkg = Join-Path $Root 'external\vcpkg'
    if ($env:VO_VCPKG_ROOT) {
        $env:VCPKG_ROOT = $env:VO_VCPKG_ROOT
    }
    elseif (Test-Path (Join-Path $externalVcpkg 'vcpkg.exe')) {
        $env:VCPKG_ROOT = $externalVcpkg
    }
    elseif (-not $env:VCPKG_ROOT -and $vsPath) {
        $env:VCPKG_ROOT = Join-Path $vsPath 'VC\vcpkg'
    }
    if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
        throw "vcpkg not found (VCPKG_ROOT='$($env:VCPKG_ROOT)'). Set VO_VCPKG_ROOT or clone vcpkg into external\vcpkg."
    }
    $env:VCPKG_DISABLE_METRICS = '1'
    Write-Step "vcpkg: $($env:VCPKG_ROOT)"

    Push-Location $Root
    try {
        # ---- Clean ----
        if ($Clean -and (Test-Path $BuildDir)) {
            Write-Step "Cleaning $BuildDir"
            Remove-Item -Recurse -Force $BuildDir
        }

        # ---- Configure ----
        if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
            Write-Step "Configuring preset $Preset (first run installs vcpkg packages; can take a while)"
            Invoke-Native 'cmake' @('--preset', $Preset)
        }

        # ---- Build ----
        $buildArgs = @('--build', '--preset', $Preset)
        if ($Target) { $buildArgs += @('--target', $Target) }
        Write-Step "Building $Preset$(if ($Target) { " target $Target" })"
        Invoke-Native 'cmake' $buildArgs

        # ---- Test ----
        if ($Test) {
            Write-Step "Running tests ($Preset)"
            Invoke-Native 'ctest' @('--preset', $Preset)
        }
    }
    finally {
        Pop-Location
    }

    $elapsed = (Get-Date) - $started
    Write-Step ("Done in {0:N1} s. Binaries: {1}" -f $elapsed.TotalSeconds, (Join-Path $BuildDir 'bin'))
    exit 0
}
catch {
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
