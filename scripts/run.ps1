[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$executable = Join-Path $outputDir 'SignalStudio.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Executable does not exist. Run scripts/build.ps1 -Configuration $Configuration first."
}
Push-Location -LiteralPath $outputDir
try {
    & $executable @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "SignalStudio exited with code $LASTEXITCODE" }
}
finally {
    Pop-Location
}
