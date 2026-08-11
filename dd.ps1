#requires -Version 5.1
<#
.SYNOPSIS
    Build-and-run helper for lib8bit.

.DESCRIPTION
    dd test    Build the emulator tests, then run them.
    dd run     Build the GUI test app, then launch it.

    Both commands build the code first. Output binaries land in bin\ using the
    naming convention <name><64|32><r|d>, e.g. test8bit64d.exe, app8bit64d.exe.

.PARAMETER Command
    'test' or 'run'.

.PARAMETER Release
    Force the Release configuration. By default 'run' builds Release and 'test'
    builds Debug.

.PARAMETER DebugBuild
    Force the Debug configuration (overrides the 'run' default of Release).

.PARAMETER Win32
    Build the 32-bit (Win32) platform instead of x64.

.EXAMPLE
    .\dd.ps1 test
    .\dd.ps1 run
    .\dd.ps1 run -DebugBuild
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('test', 'run')]
    [string]$Command,

    [switch]$Release,

    [switch]$DebugBuild,

    [switch]$Win32
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not $Command) {
    Write-Host 'Usage: .\dd.ps1 COMMAND [options]'
    Write-Host ''
    Write-Host 'Commands:'
    Write-Host '  test    Build and run the emulator tests (Debug by default)'
    Write-Host '  run     Build and launch the GUI test app (Release by default)'
    Write-Host ''
    Write-Host 'Options: -Release, -DebugBuild, -Win32'
    exit 0
}

# Resolve configuration: explicit -Release/-DebugBuild win; otherwise 'run'
# defaults to Release and 'test' defaults to Debug.
if ($Release) { $configuration = 'Release' }
elseif ($DebugBuild) { $configuration = 'Debug' }
elseif ($Command -eq 'run') { $configuration = 'Release' }
else { $configuration = 'Debug' }

# Resolve platform and the matching binary-name suffix.
$platform   = if ($Win32) { 'Win32' } else { 'x64' }
$archSuffix = if ($Win32) { '32'    } else { '64' }
$cfgSuffix  = if ($configuration -eq 'Release') { 'r' } else { 'd' }
$suffix     = "$archSuffix$cfgSuffix"


switch ($Command) {
    'test' {
        $project = Join-Path $root 'test\lib8bit-tests.vcxproj'
        $exeName = "test8bit$suffix.exe"
    }
    'run' {
        $project = Join-Path $root 'app\lib8bit-test.vcxproj'
        $exeName = "app8bit$suffix.exe"
    }
}

# Locate MSBuild via vswhere (works from any shell, no dev prompt required).
function Get-MSBuildPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at '$vswhere'. Is Visual Studio installed?"
    }

    $installPath = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.Component.MSBuild `
        -property installationPath
    if (-not $installPath) {
        throw 'Could not locate a Visual Studio installation with MSBuild.'
    }

    $candidates = @(
        (Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path $installPath 'MSBuild\Current\Bin\amd64\MSBuild.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    throw "MSBuild.exe not found under '$installPath'."
}

$msbuild = Get-MSBuildPath

Write-Host "Building $configuration|$platform ..." -ForegroundColor Cyan
& $msbuild $project /m /nologo `
    /p:Configuration=$configuration `
    /p:Platform=$platform `
    /p:SolutionDir="$root\"
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$exePath = Join-Path $root "bin\$exeName"
if (-not (Test-Path $exePath)) {
    throw "Expected binary not found: $exePath"
}

Write-Host "Launching $exeName ..." -ForegroundColor Green
& $exePath
exit $LASTEXITCODE
