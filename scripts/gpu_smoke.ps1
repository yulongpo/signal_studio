[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$OutputDirectory,
    [switch]$SoftwareRenderer
)
$ErrorActionPreference = 'Stop'

# Target connected monitor 2; preserve its friendly name and native GDI identity.
# Native monitor DPI is required; scale-factor overrides are removed for this run.
$repoRoot = Split-Path -Parent $PSScriptRoot
$configurationName = $Configuration.ToLowerInvariant()
$packageDirectory = Join-Path $repoRoot "out/vs2026-qt611-${configurationName}_bin"
$executable = Join-Path $packageDirectory 'SignalStudio.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable does not exist: $executable" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'artifacts/gpu-smoke' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$renderMode = if ($SoftwareRenderer) { 'software' } else { 'gpu' }
$renderOption = if ($SoftwareRenderer) { '--software-renderer' } else { '--require-gpu' }
$evidenceName = "signal-studio-${configurationName}-${renderMode}-screen2-4k-150"
$screenPath = Join-Path $OutputDirectory "${evidenceName}-screens.json"
$reportPath = Join-Path $OutputDirectory "${evidenceName}-report.json"
$screenshotPath = Join-Path $OutputDirectory "${evidenceName}.png"
$environmentNames = @(
    'PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM',
    'QML2_IMPORT_PATH', 'QML_IMPORT_PATH', 'QT_SCALE_FACTOR', 'QT_SCREEN_SCALE_FACTORS',
    'QT_AUTO_SCREEN_SCALE_FACTOR', 'QT_ENABLE_HIGHDPI_SCALING', 'QT_SCALE_FACTOR_ROUNDING_POLICY'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location -LiteralPath $packageDirectory
try {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $env:QT_QPA_PLATFORM = 'windows'
    & $executable --list-screens --render-report $screenPath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Monitor inventory failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $screenPath) -or (Get-Item -LiteralPath $screenPath).Length -eq 0) {
        throw "Monitor inventory was not produced: $screenPath"
    }
    Get-Content -LiteralPath $screenPath | Out-Host
    $arguments = @(
        '--demo-data', '--smoke-test', '--screen', '2', '--full-screen', '--verify-4k-150',
        '--size', '2560x1440', '--render-report', $reportPath,
        '--screenshot', $screenshotPath, $renderOption
    )
    & $executable @arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Monitor 2 4K / 150% full-screen smoke failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $reportPath)) { throw "Renderer report was not produced: $reportPath" }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.pass -ne $true -or $report.fourK150Verified -ne $true -or $report.fullScreen -ne $true) {
        throw 'The application did not confirm a successful native 4K / 150% full-screen run.'
    }
    if ($report.screen.connectedIndex -ne 2 -or
        [Math]::Abs([double]$report.screen.devicePixelRatio - 1.5) -gt 0.001 -or
        @($report.screen.pixelSize).Count -ne 2 -or $report.screen.pixelSize[0] -ne 3840 -or $report.screen.pixelSize[1] -ne 2160 -or
        @($report.logicalWindowSize).Count -ne 2 -or $report.logicalWindowSize[0] -ne 2560 -or $report.logicalWindowSize[1] -ne 1440 -or
        @($report.capturePixelSize).Count -ne 2 -or $report.capturePixelSize[0] -ne 3840 -or $report.capturePixelSize[1] -ne 2160 -or
        [Math]::Abs([double]$report.devicePixelRatio - 1.5) -gt 0.001) {
        throw 'Renderer evidence does not match connected monitor 2 at native 3840x2160 pixels and 150% DPI.'
    }
    if (-not $SoftwareRenderer -and ($report.hardwareRenderer -ne $true -or
        $report.heatmapUploads -le 0 -or $report.completedFrames -le 0 -or $report.overlayReusesHeatmap -ne $true)) {
        throw 'GPU evidence requires a hardware QRhi driver, texture upload, completed frames and overlay cache reuse.'
    }
    if ($SoftwareRenderer -and $report.hardwareRenderer -eq $true) {
        throw 'Software fallback evidence unexpectedly reported GPU rendering.'
    }
    if (-not (Test-Path -LiteralPath $screenshotPath) -or (Get-Item -LiteralPath $screenshotPath).Length -eq 0) {
        throw "Screenshot was not produced: $screenshotPath"
    }
    Write-Host "Monitor 2 ($($report.screen.name)) native 4K / 150% full-screen smoke passed ($renderMode) with system-only PATH."
    Write-Host "Report: $reportPath"
    Write-Host "Screenshot: $screenshotPath"
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
