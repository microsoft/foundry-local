# Copyright (c) Microsoft Corporation. Licensed under the MIT License.
[CmdletBinding()]
param([switch]$BootstrapMaven, [string[]]$MavenArguments = @('package'))
$ErrorActionPreference = 'Stop'
$mavenHome = Join-Path $PSScriptRoot '.mvn-local'
$maven = Get-Command mvn -ErrorAction SilentlyContinue
if ($maven) {
    $executable = $maven.Source
} else {
    $executable = Join-Path $mavenHome 'tools\apache-maven-3.9.9\bin\mvn.cmd'
    if (!(Test-Path -LiteralPath $executable)) {
        if (!$BootstrapMaven) { throw 'Maven is missing. Install Maven or explicitly pass -BootstrapMaven.' }
        $archive = Join-Path $mavenHome 'downloads\maven.zip'
        New-Item -ItemType Directory -Force (Split-Path $archive), (Join-Path $mavenHome 'tools') | Out-Null
        if (!(Test-Path -LiteralPath $archive)) {
            Invoke-WebRequest 'https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.9/apache-maven-3.9.9-bin.zip' -OutFile $archive
        }
        $expected = '8beac8d11ef208f1e2a8df0682b9448a9a363d2ad13ca74af43705549e72e74c9378823bf689287801cbbfc2f6ea9596201d19ccacfdfb682ee8a2ff4c4418ba'
        if ((Get-FileHash $archive -Algorithm SHA512).Hash -ne $expected) { throw 'Maven archive checksum mismatch' }
        Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $mavenHome 'tools') -Force
    }
}
& $executable -f (Join-Path $PSScriptRoot 'pom.xml') "-Dmaven.repo.local=$(Join-Path $mavenHome 'repository')" --batch-mode --no-transfer-progress @MavenArguments
if ($LASTEXITCODE -ne 0) { throw "Maven exited with $LASTEXITCODE" }
