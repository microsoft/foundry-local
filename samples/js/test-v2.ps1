<#
.SYNOPSIS
    Validate samples/js against a local sdk_v2/js build or a release candidate.

.DESCRIPTION
    The samples declare `"foundry-local-sdk": "latest"`, which resolves to the
    published v1 package on npmjs.com. In Local mode (the default), this script:

      1. Builds sdk_v2/cpp via build.py (RelWithDebInfo, --skip_tests). The JS
         native addon links against the C++ SDK.
      2. Builds sdk_v2/js (TS + native addon + prebuilds copy).
      3. Runs `npm pack` to produce a tarball.
      4. For each sample under samples/js/, runs `npm install <tarball>`, which
         installs the v2 tarball AS `foundry-local-sdk` (the package name inside
         the tarball wins, overriding the "latest" specifier).
        5. Optionally runs each non-GUI sample under a per-sample timeout and
            reports pass/fail. ReleaseCandidate full runs use deterministic input
            profiles where needed; interactive GUI samples are reported as skipped.

    In ReleaseCandidate mode, local build and pack steps are skipped. The script
    resolves the exact `foundry-local-sdk@<ReleaseCandidateVersion>` tarball from
    ORT-Nightly, then installs it while ordinary dependencies resolve through the
    Microsoft npm proxy. It also installs the supplied node-addon-api 8.9.1 package URL.

    Nothing about the samples' package.json files is modified. Run the script
    again any time after a rebuild to re-pack and reinstall.

.PARAMETER Sample
    Run only the named sample (folder name under samples/js/). Default: all.

.PARAMETER PackageSource
    Package source to test: Local or ReleaseCandidate. Default: Local.

.PARAMETER ReleaseCandidateVersion
    foundry-local-sdk version to install in ReleaseCandidate mode.
    Default: 2.0.0-rc1.

.PARAMETER SkipBuild
    Skip the C++ and sdk_v2/js build + pack steps. Use when iterating on the
    script itself or when you've already produced a fresh tarball. Ignored in
    ReleaseCandidate mode because local builds are always skipped.

.PARAMETER Run
    After installing, run each non-GUI sample under a timeout. ReleaseCandidate
    full runs supply deterministic arguments or stdin where needed. Interactive
    GUI samples are installed but reported as skipped. Default: install-only.

.PARAMETER VisionModel
    Model alias or variant ID passed to web-server-responses-vision-example when running it.
    Default: qwen3-vl-2b-instruct-generic-cpu:2.

.PARAMETER TimeoutSec
    Per-sample timeout when -Run is supplied. Default: 120s.

.EXAMPLE
    pwsh ./test-v2.ps1
    # Build + pack v2, install into every sample. Does not run anything.

.EXAMPLE
    pwsh ./test-v2.ps1 -Sample native-chat-completions -Run
    # Build, pack, install just one sample, then `npm start` it.

.EXAMPLE
    pwsh ./test-v2.ps1 -SkipBuild -Run
    # Reuse the existing tarball; install + smoke-run every sample.

.EXAMPLE
    pwsh ./test-v2.ps1 -PackageSource ReleaseCandidate -Sample native-chat-completions
    # Install foundry-local-sdk@2.0.0-rc1 into one sample without running local builds.

.EXAMPLE
    pwsh ./test-v2.ps1 -PackageSource ReleaseCandidate -ReleaseCandidateVersion 2.0.0-rc2 -Run
    # Install and smoke-run every sample against a specific release candidate.
#>
[CmdletBinding()]
param(
    [string] $Sample,
    [ValidateSet('Local', 'ReleaseCandidate')]
    [string] $PackageSource = 'Local',
    [ValidateNotNullOrEmpty()]
    [string] $ReleaseCandidateVersion = '2.0.0-rc1',
    [switch] $SkipBuild,
    [switch] $Run,
    [string] $VisionModel = 'qwen3-vl-2b-instruct-generic-cpu:2',
    [int]    $TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
$samplesRoot = $PSScriptRoot
$repoRoot    = Resolve-Path (Join-Path $samplesRoot '..\..')
$sdkDir      = Join-Path $repoRoot 'sdk_v2\js'
$cppDir      = Join-Path $repoRoot 'sdk_v2\cpp'
$buildPy     = Join-Path $cppDir 'build.py'
$npmRegistry = 'https://packagefeedproxy.microsoft.io/npm'
$rcRegistry  = 'https://aiinfra.pkgs.visualstudio.com/PublicPackages/_packaging/ORT-Nightly/npm/registry/'
$nodeAddonApiPackage = 'https://packagefeedproxy.microsoft.io/npm/node-addon-api/-/node-addon-api-8.9.1.tgz'

if ($PackageSource -eq 'Local') {
    if (-not (Test-Path $sdkDir)) {
        throw "Cannot find sdk_v2/js at $sdkDir"
    }
    if (-not (Test-Path $buildPy)) {
        throw "Cannot find $buildPy"
    }
}

if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    $platform = 'Windows'
    $npmCommand = 'npm.cmd'
}
elseif ($IsLinux) {
    $platform = 'Linux'
    $npmCommand = 'npm'
}
elseif ($IsMacOS) {
    $platform = 'macOS'
    $npmCommand = 'npm'
}
else {
    throw 'Unsupported platform.'
}

Write-Host "Platform: $platform" -ForegroundColor DarkGray
Write-Host "Package source: $PackageSource" -ForegroundColor DarkGray
if ($PackageSource -eq 'ReleaseCandidate') {
    Write-Host "Package registry: $rcRegistry" -ForegroundColor DarkGray
}

# ---------- 1. build C++ (the JS native addon links against the C++ SDK) ----------
if ($PackageSource -eq 'Local' -and -not $SkipBuild) {
    Write-Host "==> Building sdk_v2/cpp (RelWithDebInfo)" -ForegroundColor Cyan
    $buildArgs = @('--config', 'RelWithDebInfo', '--skip_tests')
    & python $buildPy @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed (exit $LASTEXITCODE)" }
}

# ---------- 2. build + pack JS ----------
if ($PackageSource -eq 'Local' -and -not $SkipBuild) {
    Write-Host "==> Building sdk_v2/js" -ForegroundColor Cyan
    Push-Location $sdkDir
    try {
        & $npmCommand install --registry=$npmRegistry
        if ($LASTEXITCODE -ne 0) { throw "npm install in sdk_v2/js failed" }
        & $npmCommand run build
        if ($LASTEXITCODE -ne 0) { throw "npm run build in sdk_v2/js failed" }

        # Clean stale tarballs so we always pick up the freshest one.
        Get-ChildItem -Path $sdkDir -Filter 'foundry-local-sdk-*.tgz' |
            Remove-Item -Force -ErrorAction SilentlyContinue

        Write-Host "==> Staging sdk_v2/js package contents" -ForegroundColor Cyan
        & $npmCommand run pack:prebuild
        if ($LASTEXITCODE -ne 0) { throw "npm run pack:prebuild in sdk_v2/js failed" }

        Write-Host "==> Packing sdk_v2/js" -ForegroundColor Cyan
        & $npmCommand pack
        if ($LASTEXITCODE -ne 0) { throw "npm pack in sdk_v2/js failed" }
    }
    finally {
        Pop-Location
    }
}

if ($PackageSource -eq 'Local') {
    $tarball = Get-ChildItem -Path $sdkDir -Filter 'foundry-local-sdk-*.tgz' |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $tarball) {
        throw "No tarball found in $sdkDir. Run without -SkipBuild first."
    }
    $packageSpec = $tarball.FullName
    $packageVersion = $tarball.BaseName -replace '^foundry-local-sdk-', ''
    Write-Host "Package artifact: $packageSpec" -ForegroundColor DarkGray
}
else {
    $versionedPackageSpec = "foundry-local-sdk@$ReleaseCandidateVersion"
    $packageTarballOutput = & $npmCommand view $versionedPackageSpec dist.tarball `
        --registry=$rcRegistry --replace-registry-host=never
    $packageTarballExitCode = $LASTEXITCODE
    $packageSpec = ($packageTarballOutput -join [Environment]::NewLine).Trim()
    if ($packageTarballExitCode -ne 0) {
        throw "Could not resolve $versionedPackageSpec from $rcRegistry (exit $packageTarballExitCode)"
    }
    if ([string]::IsNullOrWhiteSpace($packageSpec)) {
        throw "Could not resolve a tarball URL for $versionedPackageSpec from $rcRegistry"
    }
    $packageVersion = $ReleaseCandidateVersion
    Write-Host "Package artifact: $packageSpec" -ForegroundColor DarkGray
}
Write-Host "Package version: $packageVersion" -ForegroundColor DarkGray

# ---------- 3. discover samples ----------
$candidateDirs = Get-ChildItem -Path $samplesRoot -Directory |
    Where-Object {
        (Test-Path (Join-Path $_.FullName 'package.json'))
    }

if ($Sample) {
    $candidateDirs = $candidateDirs | Where-Object { $_.Name -eq $Sample }
    if (-not $candidateDirs) {
        throw "Sample '$Sample' not found (or has no package.json)."
    }
}

# Only touch samples that actually depend on foundry-local-sdk.
$samples = foreach ($d in $candidateDirs) {
    $pkg = Get-Content (Join-Path $d.FullName 'package.json') -Raw |
        ConvertFrom-Json
    $hasDep =
        ($pkg.dependencies         -and $pkg.dependencies.'foundry-local-sdk') -or
        ($pkg.optionalDependencies -and $pkg.optionalDependencies.'foundry-local-sdk')
    if ($hasDep) { $d }
}

if (-not $samples) {
    Write-Host "No samples depend on foundry-local-sdk. Nothing to do." -ForegroundColor Yellow
    return
}

# ---------- 4. install + (optionally) run ----------
$runArtifactsDir = $null
if ($Run) {
    $runArtifactsDir = Join-Path $sdkDir 'build\samples-run-inputs'
    New-Item -ItemType Directory -Path $runArtifactsDir -Force | Out-Null
}

$results = New-Object System.Collections.Generic.List[object]

foreach ($sampleDir in $samples) {
    $name = $sampleDir.Name
    Write-Host ""
    Write-Host "==> [$name] install" -ForegroundColor Cyan
    Push-Location $sampleDir.FullName
    $installOk = $false
    $runOk     = $null
    $note      = ''
    try {
        # Remove the old install so the selected package is the canonical source. Do not
        # suppress file-lock errors: a partial cleanup makes npm fail later with
        # a misleading EBUSY rename error.
        try {
            @('node_modules', 'package-lock.json') |
                Where-Object { Test-Path $_ } |
                Remove-Item -Recurse -Force -ErrorAction Stop
        }
        catch {
            throw "Could not clean $($sampleDir.FullName)\node_modules. Close the process holding files in that directory (on Windows, use handle.exe to identify it), then retry. $($_.Exception.Message)"
        }

        if ($PackageSource -eq 'Local') {
            # The tarball's internal package name satisfies the sample's "latest" specifier.
            & $npmCommand install --no-save --package-lock=false --registry=$npmRegistry $packageSpec 2>&1 | Write-Host
        }
        else {
            & $npmCommand install --no-save --package-lock=false --registry=$npmRegistry `
                $packageSpec $nodeAddonApiPackage 2>&1 | Write-Host
        }
        $installOk = ($LASTEXITCODE -eq 0)
        if (-not $installOk) { $note = "npm install exit $LASTEXITCODE" }

        if ($installOk -and $Run -and $name -eq 'electron-chat-application') {
            $runOk = 'SKIP'
            $note = 'interactive GUI sample; install verified, run skipped'
            Write-Host "==> [$name] run skipped ($note)" -ForegroundColor Yellow
        }
        elseif ($installOk -and $Run) {
            Write-Host "==> [$name] run (timeout ${TimeoutSec}s)" -ForegroundColor Cyan

            # NOTE: do NOT use Start-Job here. PowerShell jobs run in a child
            # PSHost with no real console — native code in foundry_local.dll
            # that touches console / stdout handles can crash with 0xC0000005
            # under Start-Job but works fine under a normal console.
            #
            # Invoke node directly (parsing the `start` script) rather than
            # `npm.cmd start`. npm.cmd is a batch wrapper, so Ctrl+C lands on
            # cmd.exe first and produces the "Terminate batch job (Y/N)?"
            # prompt before the sample's SIGINT handler can shut down cleanly.
            #
            # Let stdout inherit the parent console so the user sees live
            # output (samples write progress, prompts, model responses, etc.
            # — redirecting stdout to a file hides all of that until the run
            # finishes). Only stderr is captured to a file so we can surface
            # it on failure without interleaving it into the live output.
            $pkgJson    = Get-Content (Join-Path $sampleDir.FullName 'package.json') -Raw | ConvertFrom-Json
            $startCmd   = $pkgJson.scripts.start
            if (-not $startCmd -and $pkgJson.main) { $startCmd = "node $($pkgJson.main)" }
            if (-not $startCmd) { throw "Sample $name has neither scripts.start nor main" }
            $startParts = $startCmd -split '\s+', 2
            $exe        = $startParts[0]    # typically 'node'; may be 'npx' for TS samples
            $exeArgs    = if ($startParts.Count -gt 1) { $startParts[1] } else { '' }
            if ($name -eq 'web-server-responses-vision-example') {
                $exeArgs += " $VisionModel"
            }

            $inputFile = $null
            if ($PackageSource -eq 'ReleaseCandidate' -and -not $Sample) {
                if ($name -eq 'live-audio-transcription') {
                    $exeArgs += ' --synth'
                }
                elseif ($name -eq 'tutorial-chat-assistant') {
                    $inputFile = Join-Path $runArtifactsDir "$name.stdin.txt"
                    [System.IO.File]::WriteAllText($inputFile, "Hello`nquit`n")
                }
                elseif ($name -eq 'tutorial-tool-calling') {
                    $inputFile = Join-Path $runArtifactsDir "$name.stdin.txt"
                    [System.IO.File]::WriteAllText($inputFile, "What's the weather in Seattle?`nquit`n")
                }
            }

            # Start-Process requires a real Win32 executable; PATHEXT-based
            # shims like `npx` need to be resolved to `npx.cmd`. Prefer a
            # local node_modules/.bin shim (e.g. electron) over PATH so we
            # pick up sample-local devDependencies.
            $resolved = $null
            $localBin = Join-Path $sampleDir.FullName 'node_modules\.bin'
            foreach ($ext in @('.cmd', '.exe', '.bat')) {
                $candidate = Join-Path $localBin ($exe + $ext)
                if (Test-Path $candidate) { $resolved = $candidate; break }
            }
            if (-not $resolved) {
                try {
                    $resolved = (Get-Command $exe -CommandType Application -ErrorAction Stop |
                                 Where-Object { $_.Source -match '\.(exe|cmd|bat|com)$' } |
                                 Select-Object -First 1).Source
                } catch {}
            }
            if ($resolved) { $exe = $resolved }

            $errFile = Join-Path $runArtifactsDir "$name.stderr.log"
            Remove-Item $errFile -Force -ErrorAction SilentlyContinue
            $processArgs = @{
                FilePath = $exe
                ArgumentList = $exeArgs
                WorkingDirectory = $sampleDir.FullName
                NoNewWindow = $true
                PassThru = $true
                RedirectStandardError = $errFile
            }
            if ($inputFile) { $processArgs.RedirectStandardInput = $inputFile }
            $proc = Start-Process @processArgs
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
                if (-not $runOk) { $note = "sample run exit $exit" }
            }
            # Always surface stderr if non-empty — npm warnings go to stderr
            # too, but on failure this is usually where the real error lives.
            if ((Test-Path $errFile) -and ((Get-Item $errFile).Length -gt 0)) {
                Write-Host "--- stderr ($errFile) ---" -ForegroundColor DarkGray
                Get-Content $errFile | Select-Object -First 40 | Write-Host
            }
        }
    }
    catch {
        $note = $_.Exception.Message
        Write-Host "ERROR: $note" -ForegroundColor Red
    }
    finally {
        Pop-Location
    }

    $results.Add([pscustomobject]@{
        Sample        = $name
        PackageSource = $PackageSource
        Version       = $packageVersion
        Package       = $packageSpec
        Install       = if ($installOk) { 'OK' } else { 'FAIL' }
        Run           = if ($null -eq $runOk) { '-' } elseif ($runOk -eq 'SKIP') { 'SKIP' } elseif ($runOk) { 'OK' } else { 'FAIL' }
        Note          = $note
    })
}

# ---------- 5. summary ----------
Write-Host ""
Write-Host "==> Summary" -ForegroundColor Cyan
$results | Format-Table -AutoSize

$anyFail = $results | Where-Object { $_.Install -eq 'FAIL' -or $_.Run -eq 'FAIL' }
if ($anyFail) { exit 1 }
