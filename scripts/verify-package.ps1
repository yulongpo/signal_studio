[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Narrowband,
    [switch]$SoftwareRenderer,
    [string]$AnalysisProject,
    [string]$ReportPath
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$executable = Join-Path $outputDir 'SignalStudio.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable does not exist: $executable" }
if ($AnalysisProject -and $Narrowband) { throw 'Choose the actual analysis project or the packaged demo.' }
if ($AnalysisProject) {
    $AnalysisProject = [IO.Path]::GetFullPath($AnalysisProject)
    if ([IO.Path]::GetExtension($AnalysisProject) -ine '.ssproj') { throw 'Explicitly convert legacy v6 JSON first; AnalysisProject must be a .ssproj file.' }
}
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
    if (-not $ReportPath) {
        $ReportPath = Join-Path $repoRoot "docs/acceptance/$($Configuration.ToLowerInvariant())-${mode}4k-native${softwareSuffix}-package.json"
    }
    $reportPath = [IO.Path]::GetFullPath($ReportPath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $reportPath) -Force | Out-Null
    $arguments = @()
    if ($AnalysisProject) { $arguments += @('--project',$AnalysisProject,'--verify-narrowband-analysis') }
    elseif ($Narrowband) { $arguments += @('--narrowband-demo', '--verify-narrowband-pages') } else { $arguments += '--demo-data' }
    $arguments += @('--smoke-test', '--size', '2560x1440', '--screen', '4k', '--full-screen', '--verify-4k-150', '--render-report', $reportPath, $renderOption)
    if ($AnalysisProject) { & $executable @arguments 2>&1 | Out-File -LiteralPath ([IO.Path]::ChangeExtension($reportPath,'.process.log')) -Encoding utf8 }
    else { & $executable @arguments | Out-Host }
    if ($LASTEXITCODE -ne 0) { throw "Standalone smoke test failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $reportPath)) { throw "Renderer report was not produced: $reportPath" }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.pass -ne $true -or $report.fourK150Verified -ne $true -or $report.fullScreen -ne $true -or $report.captureHasVisibleContent -ne $true) { throw 'Native display or renderer acceptance failed.' }
    if ($Narrowband -and ($report.workspaceMode -ne 'narrowband' -or $report.narrowbandPagesVerified -ne $true -or
        @($report.narrowbandPages).Count -ne 5 -or
        (@($report.narrowbandPages.pageIndex | Sort-Object -Unique) -join ',') -ne '0,1,2,3,4')) { throw 'All five distinct narrowband pages must be verified (mask 0x1f).' }
    if ($AnalysisProject -and ($report.workspaceMode -ne 'narrowband' -or -not $report.analysis.ready -or -not $report.analysis.realSource -or -not $report.analysis.carrierLocked -or $report.analysis.eyeTraces -le 0 -or $report.analysis.pointCount -le 0)) { throw 'The real analysis project must produce recovered constellation and eye data.' }
    if (-not $SoftwareRenderer -and ($report.hardwareRenderer -ne $true -or $report.heatmapUploads -le 0 -or
        $report.gpuDataDrawCalls -le 0 -or $report.gpuVertexUploads -le 0)) { throw 'Hardware QRhi chart-data drawing and buffer uploads were not verified.' }
    if (-not $SoftwareRenderer -and $Narrowband -and
        # The unavailable recognition model does not exempt its visible real
        # spectrum from the existing GPU chart-data assertion.
        @($report.narrowbandPages | Where-Object { $_.gpuDataDrawCalls -le 0 }).Count -gt 0) { throw 'A narrowband page had no GPU chart-data draw call.' }
    $report | Add-Member -NotePropertyName standaloneProcess -NotePropertyValue ([ordered]@{executable=$executable;configuration=$Configuration;systemOnlyPath=$true;physicalMonitorAcceptanceClaimed=$false}) -Force
    $report | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Host "Standalone startup passed ($renderMode / Windows platform) from $outputDir with system-only PATH. Physical monitor identity is checked separately."
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
