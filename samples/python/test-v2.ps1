<#
.SYNOPSIS
    Validate samples/python against a locally built sdk_v2/cpp + sdk_v2/python wheel.

.DESCRIPTION
    The Python samples reference `foundry-local-sdk` in their requirements.txt,
    which resolves to PyPI by default. This script:

      1. Builds sdk_v2/cpp via build.py (RelWithDebInfo).
      2. Stages the native library into
         sdk_v2/python/src/foundry_local_sdk/_native/<rid>/ (where the wheel
         build expects it).
    3. Builds the unified wheel via `python -m build --wheel`.
      4. Creates a shared venv at sdk_v2/python/build/samples-venv/ and force-
         installs the freshly-built wheel into it.
      5. For each sample under samples/python/, runs
         `pip install --no-deps -r requirements.txt` (the SDK is already
         satisfied, so this only pulls extras like openai, langchain, pillow)
         and optionally runs `python src/app.py` under a timeout.

    Nothing about the samples' requirements.txt files is modified.

.PARAMETER Sample
    Run only the named sample (folder name under samples/python/). Default: all.

.PARAMETER SkipBuild
    Skip the C++ build *and* the wheel rebuild. Reuses whatever wheel is
    currently in sdk_v2/python/dist/. Use when iterating on sample code only.

.PARAMETER SkipWheel
    Skip rebuilding the wheel but still build C++. Use when iterating on C++
    only.

.PARAMETER PythonExe
    Python interpreter to use for building the wheel and creating the venv.
    Default: 'python'.

.PARAMETER Run
    After installing, actually run each sample (`python src/app.py`) under a
    timeout. Default behaviour is install-only.

.PARAMETER TimeoutSec
    Per-sample timeout when -Run is supplied. Default: 120s.

.EXAMPLE
    pwsh ./test-v2.ps1
    # Build C++, build wheel, install into shared venv, install each sample's
    # extra deps. Does not run anything.

.EXAMPLE
    pwsh ./test-v2.ps1 -Sample native-chat-completions -Run
    # Single sample, then `python src/app.py` it.

.EXAMPLE
    pwsh ./test-v2.ps1 -SkipWheel -Run
    # Reuse the wheel already in sdk_v2/python/dist/; install + run samples.
#>
[CmdletBinding()]
param(
    [string] $Sample,
    [switch] $SkipBuild,
    [switch] $SkipWheel,
    [string] $PythonExe = 'python',
    [switch] $Run,
    [int]    $TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
$samplesRoot = $PSScriptRoot
$repoRoot    = Resolve-Path (Join-Path $samplesRoot '..\..')
$cppDir      = Join-Path $repoRoot 'sdk_v2\cpp'
$pyDir       = Join-Path $repoRoot 'sdk_v2\python'
$buildPy     = Join-Path $cppDir 'build.py'
$distDir     = Join-Path $pyDir  'dist'
$nativeRoot  = Join-Path $pyDir  'src\foundry_local_sdk\_native'

if (-not (Test-Path $buildPy)) { throw "Cannot find $buildPy" }
if (-not (Test-Path $pyDir))   { throw "Cannot find $pyDir" }

# Platform → C++ build subdir + RID + native lib filename(s).
if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    $platform = 'Windows'
    $arch     = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $rid      = "win-$arch"
    $nativeBinSubdir = "$platform\RelWithDebInfo\bin\RelWithDebInfo"
    # On Windows the cffi extension also needs foundry_local.lib, which sits
    # in a sibling directory (one level up from bin/).
    $nativeLibSubdir = "$platform\RelWithDebInfo\RelWithDebInfo"
    $nativeFiles = @('foundry_local.dll', 'foundry_local.pdb')
    # WinML 2.x builds co-locate the registration-free ML runtime
    # (Microsoft.Windows.AI.MachineLearning.dll) next to foundry_local.dll.
    # Staged best-effort — missing-file checks below tolerate non-WinML builds
    # where it doesn't exist.
    $winmlExtraFiles = @('Microsoft.Windows.AI.MachineLearning.dll')
    $nativeLibFiles = @('foundry_local.lib')
}
elseif ($IsLinux) {
    $platform = 'Linux'
    $rid      = 'linux-x64'
    $nativeBinSubdir = 'Linux/RelWithDebInfo/bin'
    $nativeLibSubdir = $null
    $nativeFiles = @('libfoundry_local.so')
    $winmlExtraFiles = @()
    $nativeLibFiles = @()
}
elseif ($IsMacOS) {
    $platform = 'macOS'
    $rid      = 'osx-arm64'
    $nativeBinSubdir = 'macOS/RelWithDebInfo/bin'
    $nativeLibSubdir = $null
    $nativeFiles = @('libfoundry_local.dylib')
    $winmlExtraFiles = @()
    $nativeLibFiles = @()
}
else {
    throw 'Unsupported platform.'
}

if ($SkipBuild) { $SkipWheel = $true }

$pkgName = 'foundry-local-sdk'
$pkgNameSafe = $pkgName.Replace('-', '_')   # used in wheel filename matching
Write-Host "Platform: $platform / $rid / pkg=$pkgName" -ForegroundColor DarkGray

$cppBinDir = Join-Path $cppDir "build\$nativeBinSubdir"
$cppLibDir = if ($nativeLibSubdir) { Join-Path $cppDir "build\$nativeLibSubdir" } else { $null }

# ---------- 1. build C++ ----------
if (-not $SkipBuild) {
    Write-Host "==> Building sdk_v2/cpp (RelWithDebInfo)" -ForegroundColor Cyan
    $buildArgs = @('--config', 'RelWithDebInfo', '--skip_tests')
    & $PythonExe $buildPy @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed (exit $LASTEXITCODE)" }
}

# Verify the primary native lib exists.
$primaryFound = $false
foreach ($f in $nativeFiles) {
    if (Test-Path (Join-Path $cppBinDir $f)) { $primaryFound = $true; break }
}
if (-not $primaryFound) {
    throw "No foundry_local native library found in $cppBinDir."
}

# ---------- 2. stage native lib into _native/<rid>/ ----------
if (-not $SkipWheel) {
    $stageDir = Join-Path $nativeRoot $rid
    Write-Host "==> Staging native lib into $stageDir" -ForegroundColor Cyan
    if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    $staged = 0
    foreach ($f in $nativeFiles) {
        $src = Join-Path $cppBinDir $f
        if (Test-Path $src) {
            Copy-Item $src -Destination $stageDir -Force
            $staged++
        }
    }
    foreach ($f in $winmlExtraFiles) {
        $src = Join-Path $cppBinDir $f
        if (Test-Path $src) {
            Copy-Item $src -Destination $stageDir -Force
            $staged++
        }
    }
    foreach ($f in $nativeLibFiles) {
        if (-not $cppLibDir) { continue }
        $src = Join-Path $cppLibDir $f
        if (Test-Path $src) {
            Copy-Item $src -Destination $stageDir -Force
            $staged++
        }
    }
    Write-Host "  staged $staged file(s) into $stageDir" -ForegroundColor DarkGray

    # ---------- 3. build wheel ----------
    Write-Host "==> Building $pkgName wheel" -ForegroundColor Cyan

    # Clean stale dist + build dirs so we have a single fresh wheel to install.
    foreach ($d in @($distDir, (Join-Path $pyDir 'build'))) {
        if (Test-Path $d) {
            # Preserve the samples-venv inside sdk_v2/python/build/ across runs.
            if ($d -eq (Join-Path $pyDir 'build')) {
                Get-ChildItem $d -Directory | Where-Object { $_.Name -ne 'samples-venv' } |
                    ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
                Get-ChildItem $d -File -ErrorAction SilentlyContinue |
                    ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
            } else {
                Remove-Item $d -Recurse -Force
            }
        }
    }

    # Also nuke the stale untagged cffi extension that prior dev builds leave
    # next to build_cffi.py — the wheel build emits a properly tagged one.
    foreach ($staleExt in @('_cffi_bindings.pyd', '_cffi_bindings.so', '_cffi_bindings.dylib')) {
        $p = Join-Path $nativeRoot $staleExt
        if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
    }

    Push-Location $pyDir
    try {
        & $PythonExe -m pip install --upgrade build setuptools wheel "cffi>=1.16" 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) { throw "pip install build tooling failed" }

        # Scrub MSVC arch env vars that leak in from prior x86 Native Tools
        # Command Prompt invocations. setuptools' MSVC env detection picks
        # these up and silently selects HostX86\x86\cl.exe, producing a 32-bit
        # build whose cffi bindings then fail to link against the 64-bit
        # foundry_local.lib (cdecl vs __stdcall mismatches). Python itself can
        # still be 64-bit when this happens — the env vars override it.
        foreach ($leaked in @('VSCMD_ARG_TGT_ARCH', 'VSCMD_ARG_HOST_ARCH', 'Platform', '_PYTHON_HOST_PLATFORM', 'DISTUTILS_USE_SDK')) {
            if (Test-Path "env:$leaked") {
                Write-Host "Scrubbing leaked env var: $leaked=$([Environment]::GetEnvironmentVariable($leaked))" -ForegroundColor DarkYellow
                Remove-Item "env:$leaked" -ErrorAction SilentlyContinue
            }
        }

        # Validate the interpreter is 64-bit so we fail fast with a clear
        # error instead of a wall of cdecl/__stdcall linker errors.
        $bits = & $PythonExe -c "import struct; print(struct.calcsize('P')*8)"
        if ($LASTEXITCODE -ne 0 -or [int]$bits.Trim() -ne 64) {
            throw "Python at '$PythonExe' is $bits-bit; this build requires a 64-bit Python."
        }

        & $PythonExe -m build --wheel
        if ($LASTEXITCODE -ne 0) { throw "wheel build failed" }
    }
    finally {
        Pop-Location
    }
}

# Locate the wheel we just built (or a prior one when -SkipWheel).
$wheel = Get-ChildItem $distDir -Filter "$pkgNameSafe-*.whl" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $wheel) {
    throw "No $pkgNameSafe-*.whl found in $distDir. Build the wheel first (drop -SkipWheel)."
}
Write-Host "Using wheel: $($wheel.FullName)" -ForegroundColor DarkGray

# ---------- 4. shared venv ----------
$venvDir = Join-Path $pyDir 'build\samples-venv'
if (-not (Test-Path $venvDir)) {
    Write-Host "==> Creating shared venv at $venvDir" -ForegroundColor Cyan
    & $PythonExe -m venv $venvDir
    if ($LASTEXITCODE -ne 0) { throw "venv creation failed" }
}

if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    $venvPython = Join-Path $venvDir 'Scripts\python.exe'
} else {
    $venvPython = Join-Path $venvDir 'bin/python'
}
if (-not (Test-Path $venvPython)) { throw "Cannot find venv python at $venvPython" }

Write-Host "==> Installing wheel into venv (force-reinstall)" -ForegroundColor Cyan
& $venvPython -m pip install --upgrade pip 2>&1 | Write-Host
& $venvPython -m pip install --force-reinstall --no-deps $wheel.FullName 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) { throw "Wheel install failed" }
# Now install the wheel's deps (ORT / GenAI / pydantic / openai etc.) without
# disturbing the local wheel itself.
& $venvPython -m pip install $wheel.FullName 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) { throw "Wheel deps install failed" }

# ---------- 5. discover samples ----------
$candidateDirs = Get-ChildItem -Path $samplesRoot -Directory |
    Where-Object {
        (Test-Path (Join-Path $_.FullName 'requirements.txt'))
    }

if ($Sample) {
    $candidateDirs = $candidateDirs | Where-Object { $_.Name -eq $Sample }
    if (-not $candidateDirs) {
        throw "Sample '$Sample' not found (or has no requirements.txt)."
    }
}

# ---------- 6. install per-sample requirements + (optionally) run ----------
$results = New-Object System.Collections.Generic.List[object]

foreach ($sampleDir in $candidateDirs) {
    $name = $sampleDir.Name

    Write-Host ""
    Write-Host "==> [$name] install requirements" -ForegroundColor Cyan
    Push-Location $sampleDir.FullName
    $installOk = $false
    $runOk     = $null
    $note      = ''
    try {
        # Filter out the foundry-local-sdk line so pip never reaches
        # for PyPI to satisfy that name (the local wheel is already installed).
        # Filtering by line keeps environment markers / comments intact.
        $reqFile = Join-Path $sampleDir.FullName 'requirements.txt'
        $reqLines = Get-Content $reqFile | Where-Object { $_ -notmatch '^\s*foundry-local-sdk' }
        $filteredReq = Join-Path $sampleDir.FullName '.requirements-filtered.txt'
        Set-Content -Path $filteredReq -Value $reqLines -Encoding ASCII

        if ($reqLines | Where-Object { $_.Trim() -and -not $_.StartsWith('#') }) {
            & $venvPython -m pip install -r $filteredReq 2>&1 | Write-Host
            if ($LASTEXITCODE -ne 0) {
                $note = "pip install -r exit $LASTEXITCODE"
                throw $note
            }
        }
        $installOk = $true

        if ($Run) {
            $entry = Join-Path $sampleDir.FullName 'src\app.py'
            if (-not (Test-Path $entry)) {
                $note = 'no src/app.py to run'
                $runOk = $false
            } else {
                Write-Host "==> [$name] run (timeout ${TimeoutSec}s)" -ForegroundColor Cyan
                $errFile = Join-Path $sampleDir.FullName 'sample-run.err.log'
                Remove-Item $errFile -Force -ErrorAction SilentlyContinue

                $proc = Start-Process -FilePath $venvPython `
                    -ArgumentList @($entry) `
                    -WorkingDirectory $sampleDir.FullName `
                    -NoNewWindow -PassThru `
                    -RedirectStandardError $errFile
                $exited = $proc.WaitForExit($TimeoutSec * 1000)
                if (-not $exited) {
                    try { $proc.Kill($true) } catch { }
                    $runOk = $false
                    $note  = "timed out after ${TimeoutSec}s"
                    Write-Host $note -ForegroundColor Yellow
                }
                else {
                    $exit  = $proc.ExitCode
                    $runOk = ($exit -eq 0)
                    if (-not $runOk) { $note = "python exit $exit" }
                }
                if ((Test-Path $errFile) -and ((Get-Item $errFile).Length -gt 0)) {
                    Write-Host "--- stderr ($errFile) ---" -ForegroundColor DarkGray
                    Get-Content $errFile | Select-Object -First 40 | Write-Host
                }
            }
        }
    }
    catch {
        if (-not $note) { $note = $_.Exception.Message }
        Write-Host "ERROR: $note" -ForegroundColor Red
    }
    finally {
        Pop-Location
        Remove-Item -Force (Join-Path $sampleDir.FullName '.requirements-filtered.txt') -ErrorAction SilentlyContinue
    }

    $results.Add([pscustomobject]@{
        Sample  = $name
        Install = if ($installOk) { 'OK' } else { 'FAIL' }
        Run     = if ($null -eq $runOk) { '-' } elseif ($runOk) { 'OK' } else { 'FAIL' }
        Note    = $note
    })
}

# ---------- 7. summary ----------
Write-Host ""
Write-Host "==> Summary" -ForegroundColor Cyan
$results | Format-Table -AutoSize

$anyFail = $results | Where-Object { $_.Install -eq 'FAIL' -or $_.Run -eq 'FAIL' }
if ($anyFail) { exit 1 }
