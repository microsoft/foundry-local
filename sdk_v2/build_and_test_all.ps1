<#
.SYNOPSIS
    Build and test all sdk_v2 SDKs (C++, C#, Python, JS) in one shot.

.DESCRIPTION
    The simple developer "build and run all tests" one-shot script for sdk_v2.

    Order:
      1. C++    — python build.py (configure + build + test)
      2. C#     — dotnet test (builds via project references)
      3. Python — pip install -e . then pytest
      4. JS     — npm install + npm run build + npm test

    Each SDK runs in its own step. The script stops on the first failure
    unless -ContinueOnError is supplied, and prints a per-SDK pass/fail
    summary at the end.

.PARAMETER UseWinml
    Build the WinML variant across all SDKs:
      * C++:    passes --use_winml to build.py
      * C#:     passes -p:UseWinML=true to dotnet test
      * Python: sets FL_PYTHON_PACKAGE_NAME=foundry-local-sdk-winml before pip install
      * JS:     rebuilds the native addon against the WinML C++ build
    Windows only.

.PARAMETER Skip
    SDKs to skip. Any of: cpp, cs, python, js.

.PARAMETER Only
    Run only the named SDKs. Overrides -Skip. Any of: cpp, cs, python, js.

.PARAMETER ContinueOnError
    Keep going after a failure instead of aborting on the first one.

.PARAMETER SkipCppTests
    Build C++ but skip the integration test run (still builds tests).
    Useful when you only want to rebuild the native library for the
    downstream SDKs.

.EXAMPLE
    pwsh ./build_and_test_all.ps1
    # Full build + test, no WinML.

.EXAMPLE
    pwsh ./build_and_test_all.ps1 -UseWinml
    # Full build + test against the WinML variant.

.EXAMPLE
    pwsh ./build_and_test_all.ps1 -Only cpp,js -SkipCppTests
    # Rebuild C++ (skip its tests) then build + test the JS SDK.
#>
[CmdletBinding()]
param(
    [switch] $UseWinml,
    [ValidateSet('cpp', 'cs', 'python', 'js')]
    [string[]] $Skip = @(),
    [ValidateSet('cpp', 'cs', 'python', 'js')]
    [string[]] $Only,
    [switch] $ContinueOnError,
    [switch] $SkipCppTests
)

$ErrorActionPreference = 'Stop'

# Pinned: the language bindings look for native outputs from this build.
$Config = 'RelWithDebInfo'

$sdkRoot = $PSScriptRoot
$cppDir    = Join-Path $sdkRoot 'cpp'
$csDir     = Join-Path $sdkRoot 'cs'
$pythonDir = Join-Path $sdkRoot 'python'
$jsDir     = Join-Path $sdkRoot 'js'

if ($UseWinml -and -not $IsWindows) {
    throw "-UseWinml is Windows-only."
}

# Resolve which SDKs to run.
$all = @('cpp', 'cs', 'python', 'js')
if ($Only) {
    $targets = $all | Where-Object { $_ -in $Only }
} else {
    $targets = $all | Where-Object { $_ -notin $Skip }
}
if (-not $targets) {
    Write-Host "Nothing to do." -ForegroundColor Yellow
    return
}

# .NET build stays on Release; one-shot C++ config is pinned to RelWithDebInfo.
$dotnetConfig = 'Release'

$results = New-Object System.Collections.Generic.List[object]
$overallStart = Get-Date

function Invoke-Step {
    param(
        [string] $Name,
        [scriptblock] $Action
    )
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "==> [$Name] start (UseWinml=$UseWinml)" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
    $start = Get-Date
    $ok = $false
    $note = ''
    try {
        & $Action
        $ok = $true
    } catch {
        $note = $_.Exception.Message
        Write-Host "[$Name] FAILED: $note" -ForegroundColor Red
        if (-not $ContinueOnError) {
            $script:results.Add([pscustomobject]@{
                Sdk      = $Name
                Result   = 'FAIL'
                Duration = ((Get-Date) - $start).ToString('mm\:ss')
                Note     = $note
            })
            throw
        }
    }
    $script:results.Add([pscustomobject]@{
        Sdk      = $Name
        Result   = if ($ok) { 'OK' } else { 'FAIL' }
        Duration = ((Get-Date) - $start).ToString('mm\:ss')
        Note     = $note
    })
}

try {
    if ('cpp' -in $targets) {
        Invoke-Step 'cpp' {
            $args = @('build.py', '--config', $Config)
            if ($UseWinml)     { $args += '--use_winml' }
            if ($SkipCppTests) { $args += '--skip_tests' }
            Push-Location $cppDir
            try {
                # build.py drives configure + build + test by default.
                python @args
                if ($LASTEXITCODE -ne 0) { throw "C++ build.py exit $LASTEXITCODE" }
            } finally {
                Pop-Location
            }
        }
    }

    if ('cs' -in $targets) {
        Invoke-Step 'cs' {
            Push-Location $csDir
            try {
                $dotnetArgs = @(
                    'test',
                    'Microsoft.AI.Foundry.Local.SDK.sln',
                    '-c', $dotnetConfig,
                    '--nologo'
                )
                if ($UseWinml) { $dotnetArgs += '-p:UseWinML=true' }
                dotnet @dotnetArgs
                if ($LASTEXITCODE -ne 0) { throw "dotnet test exit $LASTEXITCODE" }
            } finally {
                Pop-Location
            }
        }
    }

    if ('python' -in $targets) {
        Invoke-Step 'python' {
            Push-Location $pythonDir
            try {
                # Sanity check: the cffi extension links against an x64 foundry_local.dll,
                # so the Python interpreter MUST be 64-bit. A 32-bit Python here causes
                # cl.exe to compile for x86, which produces __stdcall/__cdecl mismatches
                # when verifying the function-pointer table in foundry_local_c.h.
                $pyInfo = python -c @"
import struct, sys, sysconfig
print(struct.calcsize('P') * 8)
print(sysconfig.get_platform())
print(sys.executable)
"@
                if ($LASTEXITCODE -ne 0) { throw "python probe exit $LASTEXITCODE" }
                $bits, $plat, $exe = $pyInfo -split "`r?`n" | Where-Object { $_ }
                Write-Host "Using Python: $exe ($bits-bit, $plat)" -ForegroundColor DarkGray
                if ($bits -ne '64') {
                    throw "Python at $exe is $bits-bit; sdk_v2/python requires a 64-bit interpreter."
                }

                # On Windows, force setuptools' MSVC selection to target x64 regardless of
                # any inherited VSCMD/Platform state from a previous Developer Prompt.
                $restoreTgt  = $env:VSCMD_ARG_TGT_ARCH
                $restoreHost = $env:VSCMD_ARG_HOST_ARCH
                $restorePlat = $env:Platform
                if ($IsWindows) {
                    $env:VSCMD_ARG_TGT_ARCH  = 'x64'
                    $env:VSCMD_ARG_HOST_ARCH = 'x64'
                    $env:Platform            = 'x64'
                }

                $env:FL_PYTHON_PACKAGE_NAME =
                    if ($UseWinml) { 'foundry-local-sdk-winml' } else { 'foundry-local-sdk' }
                try {
                    python -m pip install -e '.[dev]'
                    if ($LASTEXITCODE -ne 0) { throw "pip install exit $LASTEXITCODE" }

                    python -m pytest test/ -v
                    if ($LASTEXITCODE -ne 0) { throw "pytest exit $LASTEXITCODE" }
                } finally {
                    Remove-Item Env:FL_PYTHON_PACKAGE_NAME -ErrorAction SilentlyContinue
                    if ($null -eq $restoreTgt)  { Remove-Item Env:VSCMD_ARG_TGT_ARCH  -ErrorAction SilentlyContinue } else { $env:VSCMD_ARG_TGT_ARCH  = $restoreTgt }
                    if ($null -eq $restoreHost) { Remove-Item Env:VSCMD_ARG_HOST_ARCH -ErrorAction SilentlyContinue } else { $env:VSCMD_ARG_HOST_ARCH = $restoreHost }
                    if ($null -eq $restorePlat) { Remove-Item Env:Platform            -ErrorAction SilentlyContinue } else { $env:Platform            = $restorePlat }
                }
            } finally {
                Pop-Location
            }
        }
    }

    if ('js' -in $targets) {
        Invoke-Step 'js' {
            Push-Location $jsDir
            try {
                npm install
                if ($LASTEXITCODE -ne 0) { throw "npm install exit $LASTEXITCODE" }

                # JS picks up the native library copied from the C++ build dir;
                # the WinML/non-WinML distinction is whichever C++ build ran above.
                npm run build
                if ($LASTEXITCODE -ne 0) { throw "npm run build exit $LASTEXITCODE" }

                npm test
                if ($LASTEXITCODE -ne 0) { throw "npm test exit $LASTEXITCODE" }
            } finally {
                Pop-Location
            }
        }
    }
} catch {
    # Already recorded by Invoke-Step. Fall through to summary.
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Summary  (total: $(((Get-Date) - $overallStart).ToString('mm\:ss')))" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String | Write-Host

$failed = $results | Where-Object { $_.Result -ne 'OK' }
if ($failed) {
    Write-Host "FAILED: $($failed.Sdk -join ', ')" -ForegroundColor Red
    exit 1
} else {
    Write-Host "All SDKs passed." -ForegroundColor Green
    exit 0
}
