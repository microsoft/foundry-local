<#
.SYNOPSIS
    Compile C++ samples against Local, NuGet, or Zip Foundry Local artifacts.

.DESCRIPTION
    Resolves one artifact source, validates its v2 C++ headers and native assets,
    and compiles the selected sample without changing the sample source. Local
    builds sdk_v2/cpp through its canonical build.py path. NuGet downloads the
    exact Microsoft.AI.Foundry.Local.Runtime package from ORT-Nightly. Zip
    downloads and extracts a pipeline artifact supplied with -ZipUrl.

    All generated files are placed under sdk_v2/cpp/build/sample-artifacts.

.PARAMETER ArtifactSource
    Artifact source mode: Local, NuGet, or Zip. Default: Local.

.PARAMETER Sample
    Sample folder name under samples/cpp. Default: all samples containing main.cpp.

.PARAMETER PackageVersion
    Exact Runtime package version used by NuGet mode. Default: 2.0.0-rc1.

.PARAMETER ZipUrl
    Pipeline artifact ZIP URL. Defaults to the supplied 2.0.0-rc1 build artifact.

.PARAMETER SkipBuild
    In Local mode, reuse an existing canonical sdk_v2/cpp build.

.PARAMETER Run
    Run successfully linked samples. NuGet on Windows is compile-only because
    the Runtime package intentionally does not contain foundry_local.lib.

.PARAMETER TimeoutSec
    Per-sample timeout when -Run is supplied. Default: 120 seconds.

.EXAMPLE
    pwsh ./test-v2.ps1

.EXAMPLE
    pwsh ./test-v2.ps1 -ArtifactSource NuGet -PackageVersion 2.0.0-rc1

.EXAMPLE
    pwsh ./test-v2.ps1 -ArtifactSource Zip -Run
#>
[CmdletBinding()]
param(
    [ValidateSet('Local', 'NuGet', 'Zip')]
    [string] $ArtifactSource = 'Local',
    [string] $Sample,
    [string] $PackageVersion = '2.0.0-rc1',
    [string] $ZipUrl = 'https://artprodcus3.artifacts.visualstudio.com/Abc038106-a83b-4dab-9dd3-5a41bc58f34c/0f8048ca-4cbb-4de5-a728-25d93a602394/_apis/artifact/cGlwZWxpbmVhcnRpZmFjdDovL2FpaW5mcmEvcHJvamVjdElkLzBmODA0OGNhLTRjYmItNGRlNS1hNzI4LTI1ZDkzYTYwMjM5NC9idWlsZElkLzEzNDkzOTgvYXJ0aWZhY3ROYW1lL2NwcC1zZGstdjI1/content?format=zip',
    [ValidateSet('Debug', 'MinSizeRel', 'Release', 'RelWithDebInfo')]
    [string] $Config = 'RelWithDebInfo',
    [switch] $SkipBuild,
    [switch] $Run,
    [ValidateRange(1, 86400)]
    [int] $TimeoutSec = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$samplesRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $samplesRoot '..\..')).Path
$cppRoot = Join-Path $repoRoot 'sdk_v2\cpp'
$buildRoot = Join-Path $cppRoot 'build'
$workRoot = Join-Path $buildRoot 'sample-artifacts'
$runtimePackageId = 'Microsoft.AI.Foundry.Local.Runtime'
$nugetSource = 'https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json'

function Assert-File {
    param([string] $Path, [string] $Description)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found at '$Path'."
    }
}

function Get-ArtifactFile {
    param(
        [string] $Root,
        [string] $Name,
        [string] $Description,
        [string] $PreferredPathPart,
        [switch] $Optional
    )

    $matches = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter $Name -ErrorAction SilentlyContinue |
        Sort-Object { $_.FullName.Length }, FullName)
    if ($matches.Count -eq 0) {
        if ($Optional) {
            return $null
        }

        throw "$Description '$Name' was not found beneath '$Root'."
    }

    if ($PreferredPathPart) {
        $preferredMatches = @($matches | Where-Object {
            $_.FullName -match [regex]::Escape($PreferredPathPart)
        })
        if ($preferredMatches.Count -gt 0) {
            $matches = $preferredMatches
        }
    }

    if ($matches.Count -gt 1) {
        throw "Multiple $Description candidates match '$Name' beneath '$Root'. " +
            "Provide a ZIP with an unambiguous $PreferredPathPart platform layout."
    }

    return $matches[0].FullName
}

function Assert-Headers {
    param([string] $IncludeRoot)

    $headerDir = Join-Path $IncludeRoot 'foundry_local'
    Assert-File (Join-Path $headerDir 'foundry_local_c.h') 'Foundry Local C header'
    Assert-File (Join-Path $headerDir 'foundry_local_cpp.h') 'Foundry Local C++ header'
    Assert-File (Join-Path $headerDir 'foundry_local_cpp.inline.h') 'Foundry Local inline C++ header'
}

function Get-PlatformInfo {
    $architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    if ($IsWindows -or $env:OS -eq 'Windows_NT') {
        if ($architecture -notin @('x64', 'arm64')) {
            throw "Unsupported Windows architecture '$architecture'."
        }

        return [pscustomobject]@{
            Name = 'Windows'
            Rid = "win-$architecture"
            RuntimeName = 'foundry_local.dll'
            LinkName = 'foundry_local.lib'
            ExecutableSuffix = '.exe'
            ObjectSuffix = '.obj'
        }
    }

    if ($IsLinux) {
        if ($architecture -notin @('x64', 'arm64')) {
            throw "Unsupported Linux architecture '$architecture'."
        }

        return [pscustomobject]@{
            Name = 'Linux'
            Rid = "linux-$architecture"
            RuntimeName = 'libfoundry_local.so'
            LinkName = 'libfoundry_local.so'
            ExecutableSuffix = ''
            ObjectSuffix = '.o'
        }
    }

    if ($IsMacOS) {
        if ($architecture -ne 'arm64') {
            throw "Unsupported macOS architecture '$architecture'."
        }

        return [pscustomobject]@{
            Name = 'macOS'
            Rid = 'osx-arm64'
            RuntimeName = 'libfoundry_local.dylib'
            LinkName = 'libfoundry_local.dylib'
            ExecutableSuffix = ''
            ObjectSuffix = '.o'
        }
    }

    throw 'Unsupported platform. This harness supports Windows, Linux, and macOS.'
}

function Resolve-LocalArtifacts {
    param([pscustomobject] $Platform)

    $buildPy = Join-Path $cppRoot 'build.py'
    Assert-File $buildPy 'C++ build driver'
    if (-not $SkipBuild) {
        Write-Host "==> Building sdk_v2/cpp ($Config)" -ForegroundColor Cyan
        & python $buildPy --build --config $Config
        if ($LASTEXITCODE -ne 0) {
            throw "Canonical C++ build failed with exit code $LASTEXITCODE."
        }
    }

    $platformBuild = Join-Path $buildRoot "$($Platform.Name)\$Config"
    $runtimeDir = if ($Platform.Name -eq 'Windows') {
        Join-Path $platformBuild "bin\$Config"
    } else {
        Join-Path $platformBuild 'bin'
    }
    $linkDir = if ($Platform.Name -eq 'Windows') {
        Join-Path $platformBuild $Config
    } else {
        $runtimeDir
    }

    $runtimePath = Join-Path $runtimeDir $Platform.RuntimeName
    $linkPath = Join-Path $linkDir $Platform.LinkName
    Assert-Headers (Join-Path $cppRoot 'include')
    Assert-File $runtimePath "Local $($Platform.RuntimeName)"
    Assert-File $linkPath "Local $($Platform.LinkName)"

    return [pscustomobject]@{
        IncludeRoot = Join-Path $cppRoot 'include'
        RuntimePath = $runtimePath
        LinkPath = $linkPath
        CanLink = $true
    }
}

function Resolve-NuGetArtifacts {
    param([pscustomobject] $Platform)

        $packageCache = Join-Path $workRoot "nuget\$PackageVersion"
        $packageRoot = Join-Path $packageCache 'package'
        $packagePath = Join-Path $packageCache "$runtimePackageId.$PackageVersion.nupkg"
        New-Item -ItemType Directory -Force -Path $packageCache | Out-Null

        Write-Host "==> Downloading $runtimePackageId $PackageVersion" -ForegroundColor Cyan
        $serviceIndex = Invoke-RestMethod -Uri $nugetSource
        $packageBaseAddress = $serviceIndex.resources |
                Where-Object { $_.'@type' -like 'PackageBaseAddress*' } |
                Select-Object -First 1 -ExpandProperty '@id'
        if (-not $packageBaseAddress) {
                throw "NuGet feed does not advertise a PackageBaseAddress resource: $nugetSource"
    }

        $packageIdLower = $runtimePackageId.ToLowerInvariant()
        $packageVersionLower = $PackageVersion.ToLowerInvariant()
        $packageUrl = "$($packageBaseAddress.TrimEnd('/'))/$packageIdLower/$packageVersionLower/" +
                "$packageIdLower.$packageVersionLower.nupkg"
        Invoke-WebRequest -Uri $packageUrl -OutFile $packagePath
        if (Test-Path -LiteralPath $packageRoot) {
                Remove-Item -LiteralPath $packageRoot -Recurse -Force
        }
        [System.IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $packageRoot)

    $includeRoot = Join-Path $packageRoot 'build\native\include'
    $runtimePath = Join-Path $packageRoot "runtimes\$($Platform.Rid)\native\$($Platform.RuntimeName)"
    Assert-Headers $includeRoot
    Assert-File $runtimePath "NuGet runtime asset for $($Platform.Rid)"

    $linkPath = Join-Path (Split-Path $runtimePath -Parent) $Platform.LinkName
    $canLink = Test-Path -LiteralPath $linkPath -PathType Leaf
    if ($Platform.Name -eq 'Windows' -and $canLink) {
        throw "Runtime package unexpectedly contains '$linkPath'; Windows compile-only behavior must be reviewed."
    }

    if (-not $canLink) {
        Write-Host "Validated headers and $($Platform.RuntimeName). No $($Platform.LinkName) is packaged; compile-only." `
            -ForegroundColor Yellow
    }

    return [pscustomobject]@{
        IncludeRoot = $includeRoot
        RuntimePath = $runtimePath
        LinkPath = if ($canLink) { $linkPath } else { $null }
        CanLink = $canLink
    }
}

function Resolve-ZipArtifacts {
    param([pscustomobject] $Platform)

    if ([string]::IsNullOrWhiteSpace($ZipUrl)) {
        throw 'Zip mode requires -ZipUrl with the direct pipeline artifact URL.'
    }

    $zipRoot = Join-Path $workRoot 'zip'
    $zipPath = Join-Path $zipRoot 'artifact.zip'
    $extractRoot = Join-Path $zipRoot 'extracted'
    New-Item -ItemType Directory -Force -Path $zipRoot | Out-Null

    Write-Host "==> Downloading pipeline artifact" -ForegroundColor Cyan
    Invoke-WebRequest -Uri $ZipUrl -OutFile $zipPath
    $stream = [System.IO.File]::OpenRead($zipPath)
    try {
        $signature = [byte[]]::new(4)
        $bytesRead = $stream.Read($signature, 0, $signature.Length)
    } finally {
        $stream.Dispose()
    }
    if ($bytesRead -ne 4 -or $signature[0] -ne 0x50 -or $signature[1] -ne 0x4B -or
        $signature[2] -ne 0x03 -or $signature[3] -ne 0x04) {
        throw "Pipeline artifact URL did not return a ZIP file. The Azure DevOps URL may require authentication."
    }

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $extractRoot)

    $cppHeader = Get-ArtifactFile $extractRoot 'foundry_local_cpp.h' 'C++ header'
    $headerDir = Split-Path $cppHeader -Parent
    if ((Split-Path $headerDir -Leaf) -ne 'foundry_local') {
        throw "ZIP header '$cppHeader' is not in the expected foundry_local directory."
    }
    $includeRoot = Split-Path $headerDir -Parent
    Assert-Headers $includeRoot

    $runtimePath = Get-ArtifactFile $extractRoot $Platform.RuntimeName 'runtime library' $Platform.Rid
    $linkPath = if ($Platform.LinkName -eq $Platform.RuntimeName) {
        $runtimePath
    } else {
        Get-ArtifactFile $extractRoot $Platform.LinkName 'link library' $Platform.Rid -Optional
    }
    if (-not $linkPath) {
        throw "ZIP contains $($Platform.RuntimeName) but cannot link this sample without $($Platform.LinkName)."
    }

    return [pscustomobject]@{
        IncludeRoot = $includeRoot
        RuntimePath = $runtimePath
        LinkPath = $linkPath
        CanLink = $true
    }
}

function Initialize-MsvcEnvironment {
    param([pscustomobject] $Platform)

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        return
    }

    $installationPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installationPath) {
        return
    }

    $vsDevCmd = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        return
    }

    $targetArchitecture = if ($Platform.Rid -eq 'win-arm64') { 'arm64' } else { 'x64' }
    $commandLine = "`"$vsDevCmd`" -no_logo -arch=$targetArchitecture -host_arch=x64 >nul && set"
    $environmentLines = & $env:ComSpec /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio developer environment initialization failed with exit code $LASTEXITCODE."
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
        }
    }
}

function Get-Compiler {
    if ($platform.Name -eq 'Windows') {
        $msvc = Get-Command cl.exe -ErrorAction SilentlyContinue
        if (-not $msvc) {
            Initialize-MsvcEnvironment $platform
            $msvc = Get-Command cl.exe -ErrorAction SilentlyContinue
        }

        if ($msvc) {
            return [pscustomobject]@{ Path = $msvc.Source; Kind = 'MSVC' }
        }

        throw 'No Windows host C++ compiler found. Install Visual Studio C++ tools or enter a developer shell.'
    }

    foreach ($name in @('clang++', 'g++')) {
        $compiler = Get-Command $name -ErrorAction SilentlyContinue
        if ($compiler) {
            return [pscustomobject]@{ Path = $compiler.Source; Kind = 'GNU' }
        }
    }

    throw 'No supported C++ compiler found. Enter a Visual Studio developer shell or install clang++/g++.'
}

function Invoke-SampleCompile {
    param(
        [System.IO.DirectoryInfo] $SampleDirectory,
        [pscustomobject] $Artifacts,
        [pscustomobject] $Compiler,
        [pscustomobject] $Platform
    )

    $sourcePath = Join-Path $SampleDirectory.FullName 'main.cpp'
    $outputDir = Join-Path $workRoot "output\$ArtifactSource\$($SampleDirectory.Name)"
    $objectPath = Join-Path $outputDir "main$($Platform.ObjectSuffix)"
    $executablePath = Join-Path $outputDir "$($SampleDirectory.Name)$($Platform.ExecutableSuffix)"
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    Remove-Item -LiteralPath $objectPath, $executablePath -Force -ErrorAction SilentlyContinue

    Write-Host "==> [$($SampleDirectory.Name)] compile$($(if ($Artifacts.CanLink) { ' + link' } else { ' only' }))" `
        -ForegroundColor Cyan
    if ($Compiler.Kind -eq 'MSVC') {
        $arguments = @('/nologo', '/std:c++20', '/EHsc', "/I$($Artifacts.IncludeRoot)", $sourcePath)
        if ($Artifacts.CanLink) {
            $arguments += @($Artifacts.LinkPath, '/link', "/OUT:$executablePath")
        } else {
            $arguments += @('/c', "/Fo:$objectPath")
        }
    } else {
        $arguments = @('-std=c++20', "-I$($Artifacts.IncludeRoot)", $sourcePath)
        if ($Artifacts.CanLink) {
            $arguments += @($Artifacts.LinkPath, '-o', $executablePath)
            if ($Platform.Name -ne 'Windows') {
                $arguments += "-Wl,-rpath,$(Split-Path $Artifacts.RuntimePath -Parent)"
            }
        } else {
            $arguments += @('-c', '-o', $objectPath)
        }
    }

    $compilerOutput = & $Compiler.Path @arguments 2>&1
    $compilerExitCode = $LASTEXITCODE
    $compilerOutput | Write-Host
    if ($compilerExitCode -ne 0) {
        throw "Sample '$($SampleDirectory.Name)' is not source-compatible with $ArtifactSource artifacts " +
            "(compiler exit $compilerExitCode). The sample source was intentionally left unchanged."
    }

    if ($Artifacts.CanLink) {
        return $executablePath
    }

    return $null
}

function Invoke-SampleRun {
    param([string] $ExecutablePath, [pscustomobject] $Artifacts, [pscustomobject] $Platform)

    $runDir = Split-Path $ExecutablePath -Parent
    $stdoutPath = Join-Path $runDir 'run.stdout.log'
    $stderrPath = Join-Path $runDir 'run.stderr.log'
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

    $previousPath = $env:PATH
    try {
        $env:PATH = "$(Split-Path $Artifacts.RuntimePath -Parent)$([IO.Path]::PathSeparator)$previousPath"
        $process = Start-Process -FilePath $ExecutablePath -ArgumentList '--synth' -WorkingDirectory $runDir `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
        if (-not $process.WaitForExit($TimeoutSec * 1000)) {
            try { $process.Kill($true) } catch { }
            throw "Sample timed out after $TimeoutSec seconds."
        }

        if ($process.ExitCode -ne 0) {
            if (Test-Path -LiteralPath $stderrPath) {
                Get-Content -LiteralPath $stderrPath | Select-Object -First 40 | Write-Host
            }
            throw "Sample exited with code $($process.ExitCode)."
        }
    } finally {
        $env:PATH = $previousPath
    }
}

$platform = Get-PlatformInfo
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

$sampleDirectories = @(Get-ChildItem -LiteralPath $samplesRoot -Directory | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName 'main.cpp') -PathType Leaf
})
if ($Sample) {
    $sampleDirectories = @($sampleDirectories | Where-Object Name -eq $Sample)
    if ($sampleDirectories.Count -eq 0) {
        throw "Sample '$Sample' was not found beneath '$samplesRoot'."
    }
}
if ($sampleDirectories.Count -eq 0) {
    throw "No C++ samples containing main.cpp were found beneath '$samplesRoot'."
}

Write-Host "Artifact source: $ArtifactSource" -ForegroundColor DarkGray
Write-Host "Platform:        $($platform.Name) / $($platform.Rid)" -ForegroundColor DarkGray
Write-Host "Work root:       $workRoot" -ForegroundColor DarkGray

$artifacts = switch ($ArtifactSource) {
    'Local' { Resolve-LocalArtifacts $platform }
    'NuGet' { Resolve-NuGetArtifacts $platform }
    'Zip' { Resolve-ZipArtifacts $platform }
}
$compiler = Get-Compiler
Write-Host "Compiler:        $($compiler.Path)" -ForegroundColor DarkGray
Write-Host "Include root:    $($artifacts.IncludeRoot)" -ForegroundColor DarkGray
Write-Host "Runtime:         $($artifacts.RuntimePath)" -ForegroundColor DarkGray

foreach ($sampleDirectory in $sampleDirectories) {
    $executablePath = Invoke-SampleCompile $sampleDirectory $artifacts $compiler $platform
    if ($Run) {
        if (-not $executablePath) {
            throw "-Run is unavailable for $ArtifactSource artifacts on $($platform.Name) because no link library exists."
        }

        Write-Host "==> [$($sampleDirectory.Name)] run (timeout ${TimeoutSec}s)" -ForegroundColor Cyan
        Invoke-SampleRun $executablePath $artifacts $platform
    }
}

Write-Host 'All selected sample checks passed.' -ForegroundColor Green