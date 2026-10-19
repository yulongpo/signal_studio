[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$OutputDirectory,
    [string]$IqFile
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$package = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$executable = Join-Path $package 'SignalStudioUiCapture.exe'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot "artifacts/spectral-loading/$($Configuration.ToLowerInvariant())" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$names = @('PATH','QT_QPA_PLATFORM','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS','QT_FONT_DPI','QT_AUTO_SCREEN_SCALE_FACTOR','QT_ENABLE_HIGHDPI_SCALING','QT_SCALE_FACTOR_ROUNDING_POLICY','QML2_IMPORT_PATH','QML_IMPORT_PATH')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $arguments = @('--spectral-loading', ('"' + $OutputDirectory + '"'))
    if ($IqFile) { $arguments += ('"' + [IO.Path]::GetFullPath($IqFile) + '"') }
    # The approved native acceptance explicitly requires a visible fullscreen window.
    $process = Start-Process -FilePath $executable -WorkingDirectory $package -ArgumentList $arguments -WindowStyle Normal -PassThru
    $peakWorkingSet = 0L; $peakPrivateBytes = 0L
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.HasExited) {
        if ($timer.Elapsed.TotalSeconds -gt 240) { Stop-Process -Id $process.Id; throw 'Native acceptance timed out after 240 seconds.' }
        $process.Refresh()
        $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
        $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $process.PrivateMemorySize64)
        Start-Sleep -Milliseconds 50
    }
    $process.WaitForExit()
    $reportPath = Join-Path $OutputDirectory 'spectral-loading-report.json'
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName process -NotePropertyValue ([ordered]@{
        executable = $executable; configuration = $Configuration; systemOnlyPath = $true
        exitCode = $process.ExitCode; peakWorkingSetBytes = $peakWorkingSet; peakSampledPrivateBytes = $peakPrivateBytes
        memoryNote = 'Process memory includes Qt/QRhi resources and memory-mapped source pages; IQ and analysis cache counters are separate.'
    }) -Force
    $report | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath $reportPath -Encoding utf8
    if ($process.ExitCode -ne 0 -or $report.pass -ne $true -or $report.screen.connectedIndex -ne 2 -or
        $report.screen.devicePixelRatio -ne 1.5 -or $report.screen.pixelSize.width -ne 3840 -or
        $report.screen.pixelSize.height -ne 2160 -or $report.narrowShort.paddedFrames -lt 10 -or
        $report.narrowShort.gpuDataDrawCalls -lt 1 -or $report.widePadded.gpuDataDrawCalls -lt 1) {
        throw "Spectral parameters / loading native acceptance failed: $($report.error)"
    }
    Write-Host "$Configuration spectral parameters / loading acceptance passed. Report: $reportPath"
}
finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') } }
