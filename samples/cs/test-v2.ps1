<#
.SYNOPSIS
    Validate samples/cs against a locally built sdk_v2/cpp + sdk_v2/cs.

.DESCRIPTION
    The samples reference `Microsoft.AI.Foundry.Local` (PackageReference,
    version `*-*` via Directory.Packages.props), which in turn depends on the
    `Microsoft.AI.Foundry.Local.Runtime` nupkg that carries the foundry_local
    native binary and bundles WinML acceleration on Windows.

    This script wires the samples to a local C++ build:

      1. Builds sdk_v2/cpp via build.py (RelWithDebInfo).
        2. Packs Microsoft.AI.Foundry.Local.Runtime from those native artifacts
            using sdk_v2/cpp/nuget/pack.py.
        3. Packs Microsoft.AI.Foundry.Local from sdk_v2/cs/src/ pinned to the
            locally-packed Runtime version.
      4. Writes a temporary NuGet.config that exposes a local feed alongside
         nuget.org and the AIFoundryLocal_PublicPackages Azure feed (for the
         Microsoft.ML.OnnxRuntime.Foundry / OnnxRuntimeGenAI.Foundry
         transitive deps).
      5. For each sample under samples/cs/, clears bin/obj and runs
         `dotnet restore --force --configfile <tempconfig>` followed by
         `dotnet build` (and optionally `dotnet run` under a timeout).

    All locally-packed packages use the version `99.0.0-localdev.<timestamp>`
    so they win the `*-*` floating resolution over any 0.x/2.x packages that
    may already be sitting in local-packages/.

    Nothing about the samples' csproj or Directory.Packages.props files is
    modified.

.PARAMETER Sample
    Run only the named sample (folder name under samples/cs/). Default: all.

.PARAMETER SkipBuild
    Skip the C++ build step. Use when iterating after a fresh `python
    sdk_v2/cpp/build.py` invocation.

.PARAMETER SkipPack
    Skip building the C++ SDK AND repacking the Runtime + SDK nupkgs. Reuses
    whatever this script packed previously. Implies -SkipBuild.

.PARAMETER Run
    After building, actually run each sample (dotnet run) under a timeout.
    Default behaviour is build-only.

.PARAMETER VisionModel
    Model alias or variant ID passed to foundry-local-web-server-responses-vision when running it.
    Default: qwen3-vl-2b-instruct-generic-cpu:2.

.PARAMETER TimeoutSec
    Per-sample timeout when -Run is supplied. Default: 120s.

.EXAMPLE
    pwsh ./test-v2.ps1
    # Build C++, pack Runtime + SDK, restore + build every sample.

.EXAMPLE
    pwsh ./test-v2.ps1 -Sample native-chat-completions -Run
    # Restrict to a single sample and run it after the build.

.EXAMPLE
    pwsh ./test-v2.ps1 -SkipPack -Run
    # Reuse the existing packed feed; just rebuild + run the samples.
#>
[CmdletBinding()]
param(
    [string] $Sample,
    [switch] $SkipBuild,
    [switch] $SkipPack,
    [switch] $Run,
    [string] $VisionModel = 'qwen3-vl-2b-instruct-generic-cpu:2',
    [int]    $TimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
$samplesRoot = $PSScriptRoot
$repoRoot    = Resolve-Path (Join-Path $samplesRoot '..\..')
$cppDir      = Join-Path $repoRoot 'sdk_v2\cpp'
$csSdkProj   = Join-Path $repoRoot 'sdk_v2\cs\src\Microsoft.AI.Foundry.Local.csproj'
$packPy      = Join-Path $cppDir 'nuget\pack.py'
$buildPy     = Join-Path $cppDir 'build.py'
$depsJson    = Join-Path $repoRoot 'sdk_v2\deps_versions.json'

if (-not (Test-Path $csSdkProj)) { throw "Cannot find $csSdkProj" }
if (-not (Test-Path $buildPy))   { throw "Cannot find $buildPy" }
if (-not (Test-Path $packPy))    { throw "Cannot find $packPy" }

# Platform → C++ build subdir + RID + native lib + pack.py arg name.
if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    $platform = 'Windows'
    $arch     = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $rid      = "win-$arch"
    $packArg  = if ($arch -eq 'arm64') { 'win_arm64' } else { 'win_x64' }
    $nativeBinSubdir = "$platform\RelWithDebInfo\bin\RelWithDebInfo"
}
elseif ($IsLinux) {
    $platform = 'Linux'
    $rid      = 'linux-x64'
    $packArg  = 'linux_x64'
    $nativeBinSubdir = 'Linux/RelWithDebInfo/bin'
}
elseif ($IsMacOS) {
    $platform = 'macOS'
    $rid      = 'osx-arm64'
    $packArg  = 'osx_arm64'
    $nativeBinSubdir = 'macOS/RelWithDebInfo/bin'
}
else {
    throw 'Unsupported platform.'
}

if ($SkipPack) { $SkipBuild = $true }

Write-Host "Platform: $platform / $rid" -ForegroundColor DarkGray

$cppBuildDir = Join-Path $cppDir "build\$nativeBinSubdir"
$feedDir     = Join-Path $cppDir 'build\samples-cs-feed'
$stagingDir  = Join-Path $cppDir 'build\samples-cs-feed-staging'
$configsDir  = Join-Path $cppDir 'build\samples-cs-configs'
New-Item -ItemType Directory -Force -Path $feedDir, $configsDir | Out-Null

# Locally-built packages get a high version so they win `*-*` resolution
# even when older 0.x / 2.x packages exist in local-packages/. We reuse a
# stable version across -SkipPack runs by reading whatever's currently in
# the feed dir.
$existingRuntime = Get-ChildItem $feedDir -Filter 'Microsoft.AI.Foundry.Local.Runtime.*.nupkg' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike '*.snupkg' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($SkipPack -and $existingRuntime) {
    if ($existingRuntime.Name -match '\.Runtime\.(?<v>[\d\.\-a-zA-Z]+)\.nupkg$') {
        $localVersion = $Matches.v
    } else {
        throw "Cannot parse version from $($existingRuntime.Name)"
    }
} else {
    $localVersion = "99.0.0-localdev.$(Get-Date -Format yyyyMMddHHmmss)"
}
Write-Host "Local package version: $localVersion" -ForegroundColor DarkGray

# Resolve ORT / GenAI versions for the runtime package.
$deps = Get-Content $depsJson -Raw | ConvertFrom-Json
$ortVersion   = $deps.onnxruntime.version
$genaiVersion = $deps.'onnxruntime-genai'.version

# ---------- 1. build C++ ----------
if (-not $SkipBuild) {
    Write-Host "==> Building sdk_v2/cpp (RelWithDebInfo)" -ForegroundColor Cyan
    $buildArgs = @('--config', 'RelWithDebInfo', '--skip_tests')
    & python $buildPy @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed (exit $LASTEXITCODE)" }
}

if (-not (Test-Path (Join-Path $cppBuildDir 'foundry_local.dll')) -and
    -not (Test-Path (Join-Path $cppBuildDir 'libfoundry_local.so')) -and
    -not (Test-Path (Join-Path $cppBuildDir 'libfoundry_local.dylib'))) {
    throw "No foundry_local native library found in $cppBuildDir. Did the build succeed?"
}

# ---------- 2. pack Runtime nupkg ----------
if (-not $SkipPack) {
    Write-Host "==> Packing Microsoft.AI.Foundry.Local.Runtime" -ForegroundColor Cyan

    # Clean out any prior 99.0.0-localdev.* packages so floating resolution
    # picks today's stamp. Leave other packages in feedDir untouched.
    Get-ChildItem $feedDir -Filter 'Microsoft.AI.Foundry.Local*99.0.0-localdev.*.nupkg' -ErrorAction SilentlyContinue |
        Remove-Item -Force

    $packArgs = @(
        $packPy,
        '--version',       $localVersion,
        '--package_id',    'Microsoft.AI.Foundry.Local.Runtime',
        '--ort_version',   $ortVersion,
        '--genai_version', $genaiVersion,
        "--$packArg",      $cppBuildDir,
        '--output_dir',    $feedDir,
        '--staging_dir',   $stagingDir
    )
    & python @packArgs
    if ($LASTEXITCODE -ne 0) { throw "Runtime nupkg pack failed (exit $LASTEXITCODE)" }

    # ---------- 3. pack SDK nupkg ----------
    Write-Host "==> Packing Microsoft.AI.Foundry.Local" -ForegroundColor Cyan

    # Temporary NuGet.config the SDK's own restore uses so it can resolve
    # the Runtime package we just packed plus the ORT.Foundry transitive deps.
    $sdkConfig = Join-Path $configsDir 'NuGet.sdk.config'
    @"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
    <add key="LocalSamplesFeed" value="$feedDir" />
    <add key="AIFoundryLocal_PublicPackages" value="https://pkgs.dev.azure.com/aiinfra/AIFoundryLocal/_packaging/AIFoundryLocal_PublicPackages/nuget/v3/index.json" />
  </packageSources>
</configuration>
"@ | Set-Content -Path $sdkConfig -Encoding UTF8

    # Restore + build + pack. FoundryLocalNativeBinDir is explicitly cleared
    # so the SDK csproj takes the FoundryLocalRuntimeVersion path (pulls the
    # native binary from the Runtime nupkg) — the only mode that works for
    # downstream consumers like the samples.
    & dotnet restore $csSdkProj `
        --configfile $sdkConfig `
        --force `
        /p:FoundryLocalRuntimeVersion=$localVersion `
        /p:FoundryLocalNativeBinDir=
    if ($LASTEXITCODE -ne 0) { throw "SDK restore failed" }

    & dotnet pack $csSdkProj `
        --configuration Release `
        --no-restore `
        --output $feedDir `
        /p:IsPacking=true `
        /p:PackageVersion=$localVersion `
        /p:FoundryLocalRuntimeVersion=$localVersion `
        /p:FoundryLocalNativeBinDir=
    if ($LASTEXITCODE -ne 0) { throw "SDK pack failed" }
}

# ---------- 4. write sample NuGet.config ----------
$sampleConfig = Join-Path $configsDir 'NuGet.samples.config'
@"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
    <add key="LocalSamplesFeed" value="$feedDir" />
    <add key="AIFoundryLocal_PublicPackages" value="https://pkgs.dev.azure.com/aiinfra/AIFoundryLocal/_packaging/AIFoundryLocal_PublicPackages/nuget/v3/index.json" />
  </packageSources>
  <packageSourceMapping>
    <packageSource key="nuget.org">
      <package pattern="*" />
    </packageSource>
    <packageSource key="LocalSamplesFeed">
      <package pattern="Microsoft.AI.Foundry.Local*" />
    </packageSource>
    <packageSource key="AIFoundryLocal_PublicPackages">
      <package pattern="Microsoft.ML.OnnxRuntime*" />
    </packageSource>
  </packageSourceMapping>
</configuration>
"@ | Set-Content -Path $sampleConfig -Encoding UTF8

Write-Host "Feed dir:       $feedDir" -ForegroundColor DarkGray
Write-Host "Sample config:  $sampleConfig" -ForegroundColor DarkGray

# ---------- 5. discover samples ----------
$candidateDirs = Get-ChildItem -Path $samplesRoot -Directory |
    Where-Object {
        $_.Name -ne 'Shared' -and
        (Get-ChildItem $_.FullName -Filter '*.csproj' -File -ErrorAction SilentlyContinue)
    }

if ($Sample) {
    $candidateDirs = $candidateDirs | Where-Object { $_.Name -eq $Sample }
    if (-not $candidateDirs) {
        throw "Sample '$Sample' not found (or has no .csproj)."
    }
}

# Only operate on samples that reference Microsoft.AI.Foundry.Local.
$samples = foreach ($d in $candidateDirs) {
    $csproj = Get-ChildItem $d.FullName -Filter '*.csproj' -File | Select-Object -First 1
    if (-not $csproj) { continue }
    $text = Get-Content $csproj.FullName -Raw
    if ($text -match 'Microsoft\.AI\.Foundry\.Local') {
        [pscustomobject]@{ Dir = $d; Csproj = $csproj.FullName }
    }
}

if (-not $samples) {
    Write-Host "No samples reference Microsoft.AI.Foundry.Local. Nothing to do." -ForegroundColor Yellow
    return
}

# ---------- 6. restore + build + (optionally) run ----------
$results = New-Object System.Collections.Generic.List[object]

foreach ($s in $samples) {
    $name = $s.Dir.Name
    Write-Host ""
    Write-Host "==> [$name] restore + build" -ForegroundColor Cyan
    Push-Location $s.Dir.FullName
    $buildOk = $false
    $runOk   = $null
    $note    = ''
    try {
        Remove-Item -Recurse -Force bin, obj -ErrorAction SilentlyContinue

        & dotnet restore $s.Csproj --configfile $sampleConfig --force 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            $note = "restore exit $LASTEXITCODE"
            throw $note
        }

        & dotnet build $s.Csproj --no-restore --configuration Debug 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            $note = "build exit $LASTEXITCODE"
            throw $note
        }
        $buildOk = $true

        if ($Run) {
            Write-Host "==> [$name] run (timeout ${TimeoutSec}s)" -ForegroundColor Cyan

            # Run via a child process so we can enforce a timeout. Inherit
            # stdout so interactive output (prompts, model streaming) is
            # visible live; only capture stderr to a file for surfacing on
            # failure. This mirrors how test-v2.ps1 (js) runs samples.
            $errFile = Join-Path $s.Dir.FullName 'sample-run.err.log'
            Remove-Item $errFile -Force -ErrorAction SilentlyContinue

            $runArguments = @(
                'run', '--project', $s.Csproj, '--no-build', '--no-restore', '--configuration', 'Debug'
            )
            if ($name -eq 'foundry-local-web-server-responses-vision') {
                $runArguments += @('--', $VisionModel)
            }

            $proc = Start-Process -FilePath 'dotnet' `
                -ArgumentList $runArguments `
                -WorkingDirectory $s.Dir.FullName `
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
                if (-not $runOk) { $note = "dotnet run exit $exit" }
            }
            if ((Test-Path $errFile) -and ((Get-Item $errFile).Length -gt 0)) {
                Write-Host "--- stderr ($errFile) ---" -ForegroundColor DarkGray
                Get-Content $errFile | Select-Object -First 40 | Write-Host
            }
        }
    }
    catch {
        if (-not $note) { $note = $_.Exception.Message }
        Write-Host "ERROR: $note" -ForegroundColor Red
    }
    finally {
        Pop-Location
    }

    $results.Add([pscustomobject]@{
        Sample = $name
        Build  = if ($buildOk) { 'OK' } else { 'FAIL' }
        Run    = if ($null -eq $runOk) { '-' } elseif ($runOk) { 'OK' } else { 'FAIL' }
        Note   = $note
    })
}

# ---------- 7. summary ----------
Write-Host ""
Write-Host "==> Summary" -ForegroundColor Cyan
$results | Format-Table -AutoSize

$anyFail = $results | Where-Object { $_.Build -eq 'FAIL' -or $_.Run -eq 'FAIL' }
if ($anyFail) { exit 1 }
