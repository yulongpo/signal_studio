[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Narrowband,
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
    $mode = if ($Narrowband) { 'narrowband-' } else { '' }
    $reportPath = Join-Path $repoRoot "docs/acceptance/$($Configuration.ToLowerInvariant())-${mode}4k-display2${softwareSuffix}-package.json"
    $arguments = @()
    if ($Narrowband) { $arguments += @('--narrowband-demo', '--verify-narrowband-pages') } else { $arguments += '--demo-data' }
    $arguments += @('--smoke-test', '--size', '2560x1440', '--screen', '2', '--full-screen', '--verify-4k-150', '--render-report', $reportPath, $renderOption)
    & $executable @arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Standalone smoke test failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $reportPath)) { throw "Renderer report was not produced: $reportPath" }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.pass -ne $true -or $report.fourK150Verified -ne $true -or $report.fullScreen -ne $true -or $report.captureHasVisibleContent -ne $true) { throw 'Native display or renderer acceptance failed.' }
    if ($Narrowband -and ($report.workspaceMode -ne 'narrowband' -or $report.narrowbandPagesVerified -ne $true -or @($report.narrowbandPages).Count -ne 4)) { throw 'The four narrowband pages were not all verified.' }
    if (-not $SoftwareRenderer -and ($report.hardwareRenderer -ne $true -or $report.heatmapUploads -le 0 -or
        $report.gpuDataDrawCalls -le 0 -or $report.gpuVertexUploads -le 0)) { throw 'Hardware QRhi chart-data drawing and buffer uploads were not verified.' }
    if (-not $SoftwareRenderer -and $Narrowband -and
        @($report.narrowbandPages | Where-Object { $_.gpuDataDrawCalls -le 0 }).Count -gt 0) { throw 'A narrowband page had no GPU chart-data draw call.' }
    Write-Host "Standalone startup passed ($renderMode / Windows platform) from $outputDir with system-only PATH."
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
