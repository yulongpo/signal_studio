[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string]$Project,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceBin = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$Project = [IO.Path]::GetFullPath($Project)
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out/tasks/wideband-detection'))
if (-not $OutputDirectory.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Availability copies must stay under a wideband task node directory.'
}
$missingBin = Join-Path $OutputDirectory 'missing-models-bin'
if (Test-Path -LiteralPath $missingBin) { throw 'Use a new output directory; preserve prior diagnostics.' }
New-Item -ItemType Directory -Path $missingBin -Force | Out-Null
foreach ($item in Get-ChildItem -LiteralPath $sourceBin) {
    if ($item.Name -ne 'detection-models') {
        Copy-Item -LiteralPath $item.FullName -Destination $missingBin -Recurse
    }
}
$environmentNames = @(
    'PATH', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
    'QT_SCALE_FACTOR', 'QT_SCREEN_SCALE_FACTORS', 'QT_FONT_DPI',
    'QT_DEVICE_PIXEL_RATIO', 'QT_AUTO_SCREEN_SCALE_FACTOR',
    'QT_ENABLE_HIGHDPI_SCALING', 'QT_SCALE_FACTOR_ROUNDING_POLICY',
    'QT_USE_PHYSICAL_DPI', 'CUDA_VISIBLE_DEVICES'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $env:QT_QPA_PLATFORM = 'windows'
    foreach ($mode in @('missing-models', 'no-compatible-gpu')) {
        $bin = if ($mode -eq 'missing-models') { $missingBin } else { $sourceBin }
        $env:CUDA_VISIBLE_DEVICES = if ($mode -eq 'no-compatible-gpu') { '-1' } else { $null }
        $sceneDirectory = Join-Path $OutputDirectory $mode
        Push-Location -LiteralPath $bin
        try {
            & (Join-Path $bin 'SignalStudioUiCapture.exe') --wideband-unavailable $sceneDirectory $Project $mode | Out-Host
            $captureExit = $LASTEXITCODE
        }
        finally { Pop-Location }
        $reportPath = Join-Path $sceneDirectory 'wideband-availability-report.json'
        if ($captureExit -ne 0 -or -not (Test-Path -LiteralPath $reportPath)) {
            throw "Availability native capture failed: $mode / exit $captureExit"
        }
        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($report.pass -ne $true -or $report.labelsManualHistoryUsable -ne $true -or
            $report.modelInferenceVerified -ne $false -or $report.screen.connectedIndex -ne 2 -or
            $report.screen.friendlyName -ne 'Redmi 27 NU' -or $report.screen.devicePath -notmatch 'DISPLAY6$') {
            throw "Availability evidence did not meet the required boundary: $mode"
        }
        Write-Host "Availability passed ($mode): $reportPath"
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
