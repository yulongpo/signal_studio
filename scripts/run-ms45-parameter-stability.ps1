[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 1800,

    [Parameter(Mandatory = $true)]
    [string]$EvidenceDirectory,

    [ValidateRange(1, 300)]
    [int]$SampleIntervalSeconds = 60
)

$ErrorActionPreference = 'Stop'

$resolvedBuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$testExecutable = Join-Path $resolvedBuildDirectory 'bin\signal_studio_ms04_tests.exe'
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "MS-4.5 stability executable was not found: $testExecutable"
}

$resolvedEvidenceDirectory = [System.IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force -Path $resolvedEvidenceDirectory | Out-Null
$standardOutput = Join-Path $resolvedEvidenceDirectory 'ms45_parameter_stability_30min.stdout.log'
$standardError = Join-Path $resolvedEvidenceDirectory 'ms45_parameter_stability_30min.stderr.log'
$resourceSamples = Join-Path $resolvedEvidenceDirectory 'ms45_parameter_stability_30min_resources.csv'

$arguments = @(
    '--case', 'APP-MS45-PARAMETER-STABILITY',
    '--soak-seconds', [string]$DurationSeconds
)
$process = Start-Process -FilePath $testExecutable `
    -ArgumentList $arguments `
    -WorkingDirectory $resolvedBuildDirectory `
    -WindowStyle Hidden `
    -RedirectStandardOutput $standardOutput `
    -RedirectStandardError $standardError `
    -PassThru

$samples = [System.Collections.Generic.List[object]]::new()
$startedAt = Get-Date
do {
    $process.Refresh()
    $samples.Add([pscustomobject]@{
        timestamp = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
        elapsed_seconds = [math]::Round(((Get-Date) - $startedAt).TotalSeconds, 3)
        process_id = $process.Id
        cpu_seconds = [math]::Round($process.TotalProcessorTime.TotalSeconds, 3)
        working_set_bytes = $process.WorkingSet64
        private_memory_bytes = $process.PrivateMemorySize64
        handle_count = $process.HandleCount
        thread_count = $process.Threads.Count
        state = if ($process.HasExited) { "EXIT-$($process.ExitCode)" } else { 'RUNNING' }
    })
    if (-not $process.HasExited) {
        Start-Sleep -Seconds $SampleIntervalSeconds
    }
} while (-not $process.HasExited)

$samples | Export-Csv -LiteralPath $resourceSamples -NoTypeInformation -Encoding utf8
$workingSet = $samples | Select-Object -ExpandProperty working_set_bytes
$privateMemory = $samples | Select-Object -ExpandProperty private_memory_bytes
Write-Output "MS45_STABILITY process_id=$($process.Id) exit_code=$($process.ExitCode)"
Write-Output "MS45_STABILITY samples=$($samples.Count) duration_seconds=$DurationSeconds"
Write-Output "MS45_STABILITY working_set_min=$($workingSet | Measure-Object -Minimum | Select-Object -ExpandProperty Minimum) working_set_max=$($workingSet | Measure-Object -Maximum | Select-Object -ExpandProperty Maximum)"
Write-Output "MS45_STABILITY private_memory_min=$($privateMemory | Measure-Object -Minimum | Select-Object -ExpandProperty Minimum) private_memory_max=$($privateMemory | Measure-Object -Maximum | Select-Object -ExpandProperty Maximum)"

if ($process.ExitCode -ne 0) {
    throw "MS-4.5 30-minute parameter stability test failed with exit code $($process.ExitCode)."
}
