#!/usr/bin/env pwsh
# Foundry Local 2.0.0 validation runner — Windows wrapper.
# Bootstraps prerequisites and invokes the stdlib-only orchestrator.
# Pass orchestrator flags through, e.g.:  ./run.ps1 -List  |  ./run.ps1 --sdk python
# (A leading -List is accepted as a friendly alias for --list.)
[CmdletBinding()]
param(
    [switch]$List,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Orch = Join-Path $ScriptDir 'orchestrator/run_validation.py'

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $py) {
    Write-Error 'python not found. Install Python 3.10+ and retry.'
    exit 1
}

foreach ($v in @('FOUNDRY_VALIDATION_NUGET_FEED', 'FOUNDRY_VALIDATION_NPM_REGISTRY', 'FOUNDRY_VALIDATION_PIP_INDEX')) {
    if (-not [Environment]::GetEnvironmentVariable($v)) {
        Write-Warning "`$$v is not set - cells needing that feed will be reported as 'blocked'."
    }
}

$passthru = @()
if ($List) { $passthru += '--list' }
if ($Args) { $passthru += $Args }

& $py.Source $Orch @passthru
exit $LASTEXITCODE
