[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root 'docs/acceptance/import-brand-v2' }
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$name=$Configuration.ToLowerInvariant()
$package=Join-Path $root "out/vs2026-qt611-${name}_bin"
$exe=Join-Path $package 'SignalStudioSampleFormatTests.exe'
$reportPath=Join-Path $output "load-${name}.json"
$process=Start-Process -FilePath $exe -ArgumentList @('--benchmark', ('"'+$reportPath+'"')) -WorkingDirectory $package -WindowStyle Hidden -PassThru
$peak=0L
$observations=0
while (-not $process.HasExited) {
    $process.Refresh()
    $peak=[Math]::Max($peak,$process.PeakWorkingSet64)
    ++$observations
    Start-Sleep -Milliseconds 5
}
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "Real import benchmark failed: $($process.ExitCode)" }
if ($peak -le 0 -or -not (Test-Path -LiteralPath $reportPath)) { throw 'No process memory or load evidence' }
$report=Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
$report | Add-Member -NotePropertyName peakWorkingSetBytes -NotePropertyValue $peak
$report | Add-Member -NotePropertyName memoryObservations -NotePropertyValue $observations
$report | Add-Member -NotePropertyName memoryMeasurement -NotePropertyValue 'Windows process PeakWorkingSet64 sampled during real source scan; includes reader mapping and runtime, not the UI/GPU process'
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Host "Real 64 MiB ${Configuration} scan: $($report.elapsedMilliseconds) ms; peak working set $peak bytes."
