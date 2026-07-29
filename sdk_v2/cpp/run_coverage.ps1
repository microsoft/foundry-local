# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

# run_coverage.ps1 — Collect combined code coverage from unit + integration tests.
#
# Prerequisites:
#   - OpenCppCoverage installed (https://github.com/OpenCppCoverage/OpenCppCoverage)
#   - A Debug (or RelWithDebInfo) build: python build.py --build --config Debug --parallel
#     Debug is preferred for coverage — its line tables map 1:1 to source lines, so per-line
#     hit/miss reporting is more accurate than an optimized RelWithDebInfo build.
#
# Usage:
#   .\run_coverage.ps1                         # default Debug for better matching of lines
#   .\run_coverage.ps1 -Config Debug           # specify build config
#   .\run_coverage.ps1 -SkipIntegration        # unit tests only (no model required)
#   .\run_coverage.ps1 -TestFilter "CApi*"     # pass --gtest_filter to unit tests
#   .\run_coverage.ps1 -IntegrationTestFilter "-WebServiceIntegrationTest.*"
#
# Output:
#   build\Windows\<Config>\coverage\html\index.html   — open in browser
#
param(
    [string]$Config = "Debug",
    [switch]$SkipIntegration,
    [string]$TestFilter,
    [string]$IntegrationTestFilter
)

$ErrorActionPreference = "Stop"

# --- Locate tools and binaries ---

$opencpp = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
if (-not (Test-Path $opencpp)) {
    Write-Error "OpenCppCoverage not found at $opencpp. Install from https://github.com/OpenCppCoverage/OpenCppCoverage/releases"
    exit 1
}

$buildDir  = Join-Path $PSScriptRoot "build\Windows\$Config"
$binDir    = Join-Path $buildDir "bin\$Config"
$unitExe   = Join-Path $binDir "foundry_local_tests.exe"
$integExe  = Join-Path $binDir "sdk_integration_tests.exe"
$srcDir    = Join-Path $PSScriptRoot "src"
$covDir    = Join-Path $buildDir "coverage"

if (-not (Test-Path $unitExe)) {
    Write-Error "Unit test binary not found: $unitExe`nRun: .\build.bat --build --parallel --config $Config"
    exit 1
}

New-Item -ItemType Directory -Force -Path $covDir | Out-Null

# --- Common OpenCppCoverage arguments ---

$commonArgs = @(
    "--sources", $srcDir
    "--modules", "foundry_local"
)

# --- Step 1: Unit tests → binary coverage ---

Write-Host "`n=== Collecting unit test coverage ===" -ForegroundColor Cyan

$unitCov  = Join-Path $covDir "unit.cov"
$unitArgs = $commonArgs + @("--export_type", "binary:$unitCov", "--")
$unitArgs += $unitExe

# Include DISABLED_ tests (e.g. the real-network http_download manifest test) so the unit step
# covers code paths that are network-gated off by default. Mirrors the integration step below.
$unitArgs += "--gtest_also_run_disabled_tests"

if ($TestFilter) {
    $unitArgs += "--gtest_filter=$TestFilter"
}

# Run from the test bin dir because CMake copies `testdata/` there and some
# tests resolve assets relative to std::filesystem::current_path().
Push-Location $binDir
try {
    & $opencpp @unitArgs
    if (-not (Test-Path $unitCov)) {
        Write-Error "Unit coverage file was not created: $unitCov"
        exit 1
    }

    Write-Host "Unit coverage saved to $unitCov" -ForegroundColor Green

    # --- Step 2: Integration tests → binary coverage (optional) ---

    $mergeArgs = @("--input_coverage", $unitCov)

    if (-not $SkipIntegration) {
        if (-not (Test-Path $integExe)) {
            Write-Warning "Integration test binary not found: $integExe - skipping integration coverage."
        } else {
            Write-Host "`n=== Collecting integration test coverage ===" -ForegroundColor Cyan

            $integCov  = Join-Path $covDir "integration.cov"
            $integArgs = $commonArgs + @("--export_type", "binary:$integCov", "--")
            $integArgs += $integExe

            # Include DISABLED_ tests (e.g. download tests) for maximum coverage.
            $integArgs += "--gtest_also_run_disabled_tests"

            if ($IntegrationTestFilter) {
                $integArgs += "--gtest_filter=$IntegrationTestFilter"
            }

            & $opencpp @integArgs
            if (Test-Path $integCov) {
                $mergeArgs += @("--input_coverage", $integCov)
                Write-Host "Integration coverage saved to $integCov" -ForegroundColor Green
            } else {
                Write-Warning "Integration coverage file was not created - continuing with unit only."
            }
        }
    }
}
finally {
    Pop-Location
}

# --- Step 3: Merge into HTML + Cobertura XML ---

Write-Host "`n=== Merging coverage reports ===" -ForegroundColor Cyan

$htmlDir  = Join-Path $covDir "html"
$cobFile  = Join-Path $covDir "coverage.xml"

# OpenCppCoverage merge: feed the two binary inputs and export both formats.
# HTML gives per-line browsing; Cobertura XML gives machine-readable per-line hit data
# that we can union across modules (foundry_local.dll vs foundry_local_tests.exe).
#
# OpenCppCoverage requires a child program after `--`. We use the no-arg `hostname.exe`
# rather than `cmd.exe /c exit 0` because OpenCppCoverage re-quotes each trailing token,
# turning the latter into `cmd.exe /c "exit" "0"` — cmd then fails to find a command named
# "exit" and returns exit code 1, producing a misleading error in the log. A no-arg program
# sidesteps the quoting entirely. The no-op child loads no instrumentable foundry_local
# module, so OpenCppCoverage emits a benign "No modules were selected" warning for this run;
# the real coverage comes entirely from --input_coverage, so we drop that one expected line.
$mergeAllArgs = $mergeArgs + @(
    "--export_type", "html:$htmlDir",
    "--export_type", "cobertura:$cobFile",
    "--", "hostname.exe"
)
& $opencpp @mergeAllArgs 2>&1 |
    Where-Object { $_ -notmatch "No modules were selected" } |
    ForEach-Object { Write-Host $_ }

$indexHtml = Join-Path $htmlDir "index.html"
if (-not (Test-Path $indexHtml)) {
    Write-Error "HTML report was not created."
    exit 1
}

Write-Host "HTML report: $indexHtml" -ForegroundColor Green

if (-not (Test-Path $cobFile)) {
    Write-Warning "Cobertura XML was not created - skipping combined summary."
    exit 0
}

# --- Step 4: Combined summary (union coverage across modules) ---
#
# The same source files are compiled into both foundry_local.dll (shared lib)
# and foundry_local_tests.exe (static lib). OpenCppCoverage tracks them as
# separate modules. We parse the Cobertura XML to union line hits across
# modules, giving the true combined coverage per source file.

Write-Host "`n=== Parsing Cobertura XML for combined coverage ===" -ForegroundColor Cyan

[xml]$cob = Get-Content $cobFile

# Build per-file line maps: filename -> { line_number -> $true/$false }
$fileMap = @{}

foreach ($pkg in $cob.coverage.packages.package) {
    foreach ($cls in $pkg.classes.class) {
        $filename = $cls.filename -replace '.*\\src\\', 'src\'

        if (-not $fileMap.ContainsKey($filename)) {
            $fileMap[$filename] = @{}
        }

        foreach ($line in $cls.lines.line) {
            $num  = $line.number
            $hits = [int]$line.hits

            if ($fileMap[$filename].ContainsKey($num)) {
                # Union: covered in ANY module means covered
                if ($hits -gt 0) {
                    $fileMap[$filename][$num] = $true
                }
            } else {
                $fileMap[$filename][$num] = ($hits -gt 0)
            }
        }
    }
}

# Compute per-file totals
$results = @()
$totalCovered = 0
$totalLines   = 0

foreach ($file in $fileMap.Keys) {
    $lines = $fileMap[$file]
    $cov = @($lines.Values | Where-Object { $_ }).Count
    $tot = $lines.Count
    $pct = if ($tot -gt 0) { [math]::Round(100 * $cov / $tot) } else { 0 }
    $results += [PSCustomObject]@{ File = $file; Covered = $cov; Total = $tot; Pct = $pct }
    $totalCovered += $cov
    $totalLines   += $tot
}

$totalPct = if ($totalLines -gt 0) { [math]::Round(100 * $totalCovered / $totalLines) } else { 0 }

Write-Host "`n=== Combined coverage: $totalCovered / $totalLines lines ($totalPct%) ===" -ForegroundColor Cyan
Write-Host "(union of foundry_local.dll + foundry_local_tests.exe)" -ForegroundColor DarkGray
Write-Host ""
Write-Host ("{0,-65} {1,8} {2,8} {3,6}" -f "File", "Covered", "Total", "Rate")
Write-Host ("-" * 90)

$results | Sort-Object Pct | ForEach-Object {
    $color = if ($_.Pct -lt 50) { "Red" } elseif ($_.Pct -lt 80) { "Yellow" } else { "Green" }
    Write-Host ("{0,-65} {1,8} {2,8} {3,5}%" -f $_.File, $_.Covered, $_.Total, $_.Pct) -ForegroundColor $color
}

Write-Host ""
Write-Host "Cobertura XML: $cobFile" -ForegroundColor Green
