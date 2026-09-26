# Copyright (c) Microsoft Corporation. Licensed under the MIT License.
[CmdletBinding()]
param([switch]$BootstrapMaven, [string[]]$MavenArguments = @('clean', 'verify'))
$ErrorActionPreference = 'Stop'
$mavenHome = Join-Path $PSScriptRoot 'build\maven'
$mavenTools = Join-Path $mavenHome 'tools'
$mavenDistribution = Join-Path $mavenTools 'apache-maven-3.9.9'
$mavenExecutable = if ($IsWindows) { 'mvn.cmd' } else { 'mvn' }
$bootstrappedMaven = Join-Path (Join-Path $mavenDistribution 'bin') $mavenExecutable
$maven = Get-Command mvn -ErrorAction SilentlyContinue
if ($maven) {
    $executable = $maven.Source
} else {
    $executable = $bootstrappedMaven
    if (!(Test-Path -LiteralPath $executable)) {
        if (!$BootstrapMaven) { throw 'Maven is missing. Install Maven or explicitly pass -BootstrapMaven.' }
        $archive = Join-Path $mavenHome 'downloads\maven.zip'
        New-Item -ItemType Directory -Force (Split-Path $archive), $mavenTools | Out-Null
        if (!(Test-Path -LiteralPath $archive)) {
            $uri = 'https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.9/' +
                'apache-maven-3.9.9-bin.zip'
            Invoke-WebRequest $uri -OutFile $archive
        }
        $expected = '8beac8d11ef208f1e2a8df0682b9448a9a363d2ad13ca74af43705549e72e74' +
            'c9378823bf689287801cbbfc2f6ea9596201d19ccacfdfb682ee8a2ff4c4418ba'
        if ((Get-FileHash $archive -Algorithm SHA512).Hash -ne $expected) { throw 'Maven archive checksum mismatch' }
        Expand-Archive -LiteralPath $archive -DestinationPath $mavenTools -Force
    }
    if (!$IsWindows) {
        & chmod u+x $executable
        if ($LASTEXITCODE -ne 0) { throw "Could not make Maven executable: $executable" }
    }
}
$arguments = @(
    '-f'
    (Join-Path $PSScriptRoot 'pom.xml')
    "-Dmaven.repo.local=$(Join-Path $mavenHome 'repository')"
    '--batch-mode'
    '--no-transfer-progress'
) + $MavenArguments
& $executable @arguments
if ($LASTEXITCODE -ne 0) { throw "Maven exited with $LASTEXITCODE" }
