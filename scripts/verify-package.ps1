[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$SoftwareRenderer
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$executable = Join-Path $outputDir 'SignalStudio.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable does not exist: $executable" }
$environmentNames = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_SCALE_FACTOR', 'QT_SCREEN_SCALE_FACTORS', 'QT_AUTO_SCREEN_SCALE_FACTOR', 'QT_ENABLE_HIGHDPI_SCALING', 'QT_SCALE_FACTOR_ROUNDING_POLICY', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location -LiteralPath $outputDir
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    foreach ($name in $environmentNames | Where-Object { $_ -ne 'PATH' }) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:QT_QPA_PLATFORM = 'windows'
    $renderOption = if ($SoftwareRenderer) { '--software-renderer' } else { '--require-gpu' }
    $renderMode = if ($SoftwareRenderer) { 'software' } else { 'GPU-required' }
    $softwareSuffix = if ($SoftwareRenderer) { '-software' } else { '' }
    $reportPath = Join-Path $repoRoot "docs/acceptance/$($Configuration.ToLowerInvariant())-4k-display2${softwareSuffix}-package.json"
    & $executable --demo-data --smoke-test --size 2560x1440 --screen 2 --full-screen --verify-4k-150 --render-report $reportPath $renderOption | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Standalone smoke test failed with exit code $LASTEXITCODE" }
    Write-Host "Standalone startup passed ($renderMode / Windows platform) from $outputDir with system-only PATH."
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
