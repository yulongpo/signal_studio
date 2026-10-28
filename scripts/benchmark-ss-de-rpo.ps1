# Cold is three fresh SignalStudioDetectionBenchmark.exe processes, each with
# --formal-commit. Warm is one resident process: --resident-repeat N is an
# excluded warmup plus N measured redetections, one QCoreApplication, and a
# fresh Worker per run. This script never relabels fresh processes as warm.
# Exit 0 only when every requested run passed. GPU work stays off unless -AllowGpu.
[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
  [ValidateSet('Cold', 'Warm', 'Both')][string]$Mode = 'Both',
  [Parameter(Mandatory)][string]$OutputDirectory,
  [string]$ExecutableDir,
  [string]$RequestJson,
  [string]$BaselineDirectory = 'D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N0',
  [string]$AcceptanceExecutable,
  [ValidateRange(3, 3)][int]$Repeats = 3,
  [switch]$AllowGpu,
  [Alias('Native')][switch]$NativeUi,
  [string]$SourceRevision,
  [ValidateSet('sample', 'roi', 'coarse', 'none')][string]$Strategy = 'sample',
  [ValidateRange(0, 2048)][int]$RawHeadBudgetMiB = 0,
  [ValidateRange(1, 16)][int]$Batch = 2,
  [ValidateRange(30, 7200)][int]$TimeoutSeconds = 900
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Assert-OwnedOutput([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  $taskRoot = [IO.Path]::GetFullPath((Join-Path $repo 'out/tasks/ss-de-rpo'))
  $prefix = $taskRoot.TrimEnd('\') + '\'
  if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Output must be below this worktree's out/tasks/ss-de-rpo: $full"
  }
  return $full
}

function Read-Baseline([string]$Directory) {
  $candidates = @(
    (Join-Path $Directory 'BASELINE_COLD.json'),
    (Join-Path $repo 'reference/SS-DE-RPO/_delivery/performance/BASELINE_COLD.json'),
    'D:/project/signal_studio_rebuild/reference/SS-DE-RPO/_delivery/performance/BASELINE_COLD.json'
  )
  foreach ($path in $candidates) {
    if ($path -and (Test-Path -LiteralPath $path)) {
      $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
      return [ordered]@{
        path = [IO.Path]::GetFullPath($path)
        readOnly = $true
        schema = [string]$json.schema
        applicationCold = [bool]$json.applicationCold
        workerCold = [bool]$json.workerCold
        osCacheState = [string]$json.osCacheState
        runs = @($json.runs | ForEach-Object {
          [ordered]@{ caseId = [string]$_.caseId; pass = [bool]$_.pass; elapsedMs = $_.elapsedMs; lifecycle = [string]$_.lifecycle }
        })
      }
    }
  }
  return [ordered]@{ path = $null; readOnly = $true; schema = $null; runs = @() }
}

function Start-ResourceSampler([string]$CsvPath, [int]$RootProcessId) {
  $names = @('SignalStudioDetectionBenchmark', 'SignalStudioDetectionNativeBenchmark', 'SignalStudioDetectionWorker')
  return Start-Job -ScriptBlock {
    param($CsvPath, $Names, $RootProcessId)
    $header = $false
    $gpuProbe = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($CsvPath))
    while ($true) {
      if (-not (Get-Process -Id $RootProcessId -ErrorAction SilentlyContinue)) { break }
      $children = @(Get-CimInstance Win32_Process -Filter "Name='SignalStudioDetectionWorker.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -eq $RootProcessId } | Select-Object -ExpandProperty ProcessId)
      $rows = @()
      $gpuValues = @()
      if ($gpuProbe) {
        $gpuValues = @(& $gpuProbe.Source --query-gpu=index,name,memory.used,utilization.gpu --format=csv,noheader,nounits 2>$null |
          Select-Object -First 1 | ForEach-Object { $_ -split ',\s*' })
      }
      foreach ($name in $Names) {
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
          if ($process.Id -ne $RootProcessId -and $process.Id -notin $children) { continue }
          try {
            $rows += [pscustomobject]@{
              utc = [DateTime]::UtcNow.ToString('o')
              processName = $process.ProcessName
              processId = $process.Id
              workingSet64 = $process.WorkingSet64
              peakWorkingSet64 = $process.PeakWorkingSet64
              privateMemorySize64 = $process.PrivateMemorySize64
              totalProcessorTimeMs = [int64]$process.TotalProcessorTime.TotalMilliseconds
              gpuIndex = $(if ($gpuValues.Count -eq 4) { $gpuValues[0] } else { '' })
              gpuName = $(if ($gpuValues.Count -eq 4) { $gpuValues[1] } else { '' })
              gpuUsedMiB = $(if ($gpuValues.Count -eq 4) { $gpuValues[2] } else { '' })
              gpuUtilizationPercent = $(if ($gpuValues.Count -eq 4) { $gpuValues[3] } else { '' })
              volumeAvailableBytes = $drive.AvailableFreeSpace
            }
          } catch { }
        }
      }
      if ($rows.Count -gt 0) {
        if (-not $header) {
          $rows | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding utf8
          $header = $true
        } else {
          $rows | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding utf8 -Append
        }
      }
      Start-Sleep -Milliseconds 200
    }
  } -ArgumentList $CsvPath, $names, $RootProcessId
}

function Get-WorkerIds([string]$CsvPath) {
  if (-not (Test-Path -LiteralPath $CsvPath)) { return @() }
  return @((Import-Csv -LiteralPath $CsvPath | Where-Object { $_.processName -eq 'SignalStudioDetectionWorker' } |
    Select-Object -ExpandProperty processId -Unique))
}

function Assert-PublicRawOff([string]$Path) {
  $json = Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json
  $names = @($json.PSObject.Properties.Name)
  if ($names -contains 'legacyCombinedEvidence') {
    throw "Public request JSON contains runtime-only legacyCombinedEvidence: $Path"
  }
  if ($json.retainRawModelOutput -ne $false) {
    throw "Current raw-OFF request must set retainRawModelOutput false: $Path"
  }
  if ([string]$json.rawHeadBudgetBytes -ne '0') {
    throw "Current raw-OFF request must set rawHeadBudgetBytes `"0`": $Path"
  }
}

function Get-MarkSteadyNs($Measurement, [string]$Name) {
  $marks = @($Measurement.run.performance.timeline.marks)
  if (-not $marks) { return $null }
  $hit = @($marks | Where-Object { $_.name -eq $Name }) | Select-Object -First 1
  if (-not $hit -or -not $hit.steadyNs) { return $null }
  return [string]$hit.steadyNs
}

function Get-BenchmarkArguments([string]$Request, [string]$OutputPath, [switch]$Resident) {
  if ($NativeUi) {
    $arguments = @('--request',$Request,'--output',$OutputPath)
    if ($Resident) { $arguments += @('--resident-repeat', "$Repeats") }
    return $arguments
  }
  # Do not pass --raw-head-budget-mib. Zero on that flag forces legacy combined
  # evidence inside the benchmark. The public request already says raw-OFF.
  $arguments = @(
    '--request', $Request,
    '--output', $OutputPath,
    '--strategy', $Strategy,
    '--batch', "$Batch",
    '--dsp-compute', 'gpu',
    '--timeout', "$TimeoutSeconds",
    '--formal-commit'
  )
  if ($Resident) { $arguments += @('--resident-repeat', "$Repeats") }
  return $arguments
}

$output = Assert-OwnedOutput $OutputDirectory
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force -ErrorAction SilentlyContinue).Count) {
  throw "Use a new/empty output directory: $output"
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$baseline = Read-Baseline $BaselineDirectory
$bin = if ($ExecutableDir) { [IO.Path]::GetFullPath($ExecutableDir) }
       else { Join-Path $repo ("out/vs2026-qt611-" + $Configuration.ToLowerInvariant() + "_bin") }
if (-not $AcceptanceExecutable) {
  $AcceptanceExecutable = Join-Path $bin 'SignalStudioSsDeRpoAcceptanceTests.exe'
}
if (-not $RequestJson) {
  $candidate = Join-Path $BaselineDirectory 'request-raw-off-v2.json'
  if (Test-Path -LiteralPath $candidate) { $RequestJson = [IO.Path]::GetFullPath($candidate) }
}

$benchmarkExe = Join-Path $bin $(if ($NativeUi) { 'SignalStudioDetectionNativeBenchmark.exe' } else { 'SignalStudioDetectionBenchmark.exe' })
$coldArguments = @()
$warmArguments = @()
if ($RequestJson) {
  $RequestJson = (Get-Item -LiteralPath $RequestJson).FullName
  Assert-PublicRawOff $RequestJson
  $coldArguments = @(Get-BenchmarkArguments $RequestJson (Join-Path $output 'cold-run-01\native'))
  $warmArguments = @(Get-BenchmarkArguments $RequestJson (Join-Path $output 'resident') -Resident)
}
$coldRuns = @()
$warm = [ordered]@{
  requested = ($Mode -in @('Warm', 'Both'))
  status = 'NOT_REQUESTED'
  residentApplication = $false
  newProcessPerRun = $false
  relabelledFreshProcess = $false
  workerProcessIds = @()
  distinctWorkerProcessIds = $false
  reason = 'Warm was not requested.'
  protocol = 'one SignalStudioDetectionBenchmark.exe --resident-repeat 3 --formal-commit; excluded warmup plus 3 measured redetections; fresh Worker and context per run'
  arguments = $warmArguments
  t_pipeline_terminal = $null
  t_terminal = $null
  acceptanceExitCode = $null
}
$launchedGpu = $false
$errorText = $null

try {
  if (-not $AllowGpu) {
    if ($Mode -in @('Cold', 'Both')) {
      $coldRuns += [ordered]@{
        index = 0; pass = $false; status = 'BLOCKED_EXCLUSIVE_SLOT'; residentApplication = $false
        newProcessPerRun = $false; workerProcessIds = @()
        t_pipeline_terminal = $null; t_terminal = $null
        arguments = $coldArguments
        reason = 'Cold was not started. Each cold run would be a fresh SignalStudioDetectionBenchmark.exe with --formal-commit. --raw-head-budget-mib is not passed.'
      }
    }
    if ($warm.requested) {
      $warm.status = 'BLOCKED_EXCLUSIVE_SLOT'
      $warm.reason = 'Warm was not started. The intended command is one resident SignalStudioDetectionBenchmark.exe with --resident-repeat 3 and --formal-commit. Separate processes are not warm. The acceptance --resident-redetect stub is not the measurement.'
    }
  } else {
    if (-not $RequestJson) {
      $missing = [ordered]@{
        index = 0; pass = $false; status = 'BLOCKED_MISSING_REQUEST'; residentApplication = $false
        newProcessPerRun = $false; workerProcessIds = @()
        t_pipeline_terminal = $null; t_terminal = $null
        reason = 'No RequestJson and N0 request-raw-off-v2.json was not found. No process was started.'
      }
      if ($Mode -in @('Cold', 'Both')) { $coldRuns += $missing }
      if ($warm.requested) {
        $warm.status = 'BLOCKED_MISSING_REQUEST'
        $warm.reason = $missing.reason
      }
    } elseif (-not (Test-Path -LiteralPath $benchmarkExe)) {
      $missingExe = "SignalStudioDetectionBenchmark.exe is absent: $benchmarkExe. No process was started."
      if ($Mode -in @('Cold', 'Both')) {
        $coldRuns += [ordered]@{
          index = 0; pass = $false; status = 'BLOCKED_MISSING_INPUT'; residentApplication = $false
          newProcessPerRun = $false; workerProcessIds = @()
          t_pipeline_terminal = $null; t_terminal = $null; reason = $missingExe
        }
      }
      if ($warm.requested) { $warm.status = 'BLOCKED_MISSING_INPUT'; $warm.reason = $missingExe }
    } else {
      if ($Mode -in @('Cold', 'Both')) {
        for ($index = 1; $index -le $Repeats; $index++) {
          $case = Join-Path $output ('cold-run-{0:D2}' -f $index)
          $native = Join-Path $case 'native'
          New-Item -ItemType Directory -Path $case -Force | Out-Null
          $resourceCsv = Join-Path $output ('cold-run-{0:D2}-resources.csv' -f $index)
          $stdout = Join-Path $case 'stdout.txt'
          $stderr = Join-Path $case 'stderr.txt'
          $arguments = @(Get-BenchmarkArguments $RequestJson $native)
          $sampler = $null
          $runError = $null
          $process = $null
          $benchmarkProcessId = $null
          try {
            $process = Start-Process -FilePath $benchmarkExe -ArgumentList $arguments `
              -WorkingDirectory $bin -WindowStyle Hidden -PassThru `
              -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            $benchmarkProcessId = $process.Id
            $sampler = Start-ResourceSampler $resourceCsv $process.Id
            $launchedGpu = $true
            if (-not $process.WaitForExit(($TimeoutSeconds + 60) * 1000)) {
              $process.Kill($true)
              $null = $process.WaitForExit()
              $runError = 'cold benchmark exceeded the timeout and was stopped'
            }
            $process.Refresh()
          } catch {
            $runError = $_.Exception.Message
            $launchedGpu = $true
          } finally {
            if ($sampler) {
              Stop-Job $sampler -ErrorAction SilentlyContinue | Out-Null
              Remove-Job $sampler -Force -ErrorAction SilentlyContinue | Out-Null
            }
            if ($process) { $process.Dispose() }
          }
          $measurementPath = Join-Path $native 'measurement.json'
          $measurement = $null
          if (Test-Path -LiteralPath $measurementPath) {
            $measurement = Get-Content -LiteralPath $measurementPath -Raw -Encoding utf8 | ConvertFrom-Json
          }
          $workers = @(Get-WorkerIds $resourceCsv)
          $passed = -not $runError -and $measurement -and $measurement.pass -eq $true
          $coldRuns += [ordered]@{
            index = $index
            pass = [bool]$passed
            status = $(if ($passed) { 'PASS' } else { 'FAIL' })
            residentApplication = $false
            newProcessPerRun = $true
            benchmarkProcessId = $benchmarkProcessId
            workerProcessIds = $workers
            workerPidObserved = ($workers.Count -gt 0)
            resourceCsv = $resourceCsv
            measurement = $(if (Test-Path -LiteralPath $measurementPath) { $measurementPath } else { $null })
            arguments = $arguments
            t_pipeline_terminal = $(if ($measurement) { Get-MarkSteadyNs $measurement 't_pipeline_terminal' } else { $null })
            t_terminal = $(if ($measurement) { Get-MarkSteadyNs $measurement 't_terminal' } else { $null })
            error = $runError
            evidenceClass = 'tensorrt'
            syntheticRepeatedFrames = $false
            formalSourceUntouched = $true
          }
        }
      }
      if ($warm.requested) {
        $resident = Join-Path $output 'resident'
        $resourceCsv = Join-Path $output 'resident-resources.csv'
        $stdout = Join-Path $output 'resident-stdout.txt'
        $stderr = Join-Path $output 'resident-stderr.txt'
        $arguments = @(Get-BenchmarkArguments $RequestJson $resident -Resident)
        $warm.arguments = $arguments
        $sampler = $null
        $runError = $null
        $process = $null
        try {
          $process = Start-Process -FilePath $benchmarkExe -ArgumentList $arguments `
            -WorkingDirectory $bin -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
          $launchedGpu = $true
          $sampler = Start-ResourceSampler $resourceCsv $process.Id
          if (-not $process.WaitForExit(($TimeoutSeconds + 180) * 1000)) {
            $process.Kill($true)
            $null = $process.WaitForExit()
            $runError = 'resident benchmark exceeded the timeout and was stopped'
          }
          $process.Refresh()
        } catch {
          $runError = $_.Exception.Message
          $launchedGpu = $true
        } finally {
          if ($sampler) {
            Stop-Job $sampler -ErrorAction SilentlyContinue | Out-Null
            Remove-Job $sampler -Force -ErrorAction SilentlyContinue | Out-Null
          }
        }
        $summaryPath = Join-Path $resident 'resident-summary.json'
        $summary = $null
        if (Test-Path -LiteralPath $summaryPath) {
          $summary = Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8 | ConvertFrom-Json
        }
        $workers = @(Get-WorkerIds $resourceCsv)
        $distinct = (@($workers | Select-Object -Unique).Count -ge $Repeats) -and ($workers.Count -ge $Repeats)
        $oneProcess = $null -ne $process
        $residentOk = $oneProcess -and $summary -and $summary.applicationResident -eq $true -and
          $summary.pass -eq $true -and $distinct -and -not $runError
        $warm.residentApplication = [bool]($summary -and $summary.applicationResident -eq $true -and $oneProcess)
        $warm.newProcessPerRun = $false
        $warm.relabelledFreshProcess = $false
        $warm.workerProcessIds = $workers
        $warm.distinctWorkerProcessIds = [bool]$distinct
        $warm.benchmarkProcessId = $(if ($process) { $process.Id } else { $null })
        $warm.resourceCsv = $resourceCsv
        $warm.summary = $(if (Test-Path -LiteralPath $summaryPath) { $summaryPath } else { $null })
        $warm.error = $runError
        $warm.status = $(if ($residentOk) { 'PASS' } else { 'FAIL' })
        $warm.reason = $(if ($residentOk) {
          'One resident process completed --resident-repeat with distinct Worker process ids.'
        } else {
          'Resident --resident-repeat did not record one applicationResident parent and distinct Worker process ids. Fresh processes were not relabelled warm.'
        })
        $pipeline = @()
        $business = @()
        if ($summary -and $summary.runs) {
          foreach ($item in @($summary.runs)) {
            $measured = $null
            if ($item.measurement -and (Test-Path -LiteralPath $item.measurement)) {
              $measured = Get-Content -LiteralPath $item.measurement -Raw -Encoding utf8 | ConvertFrom-Json
            }
            $pipeline += ,$(if ($measured) { Get-MarkSteadyNs $measured 't_pipeline_terminal' } else { $null })
            $business += ,$(if ($measured) { Get-MarkSteadyNs $measured 't_terminal' } else { $null })
          }
        }
        $warm.t_pipeline_terminal = $pipeline
        $warm.t_terminal = $business
        if ($process) { $process.Dispose() }
      }
    }
  }
} catch {
  $errorText = $_.Exception.Message
}

$coldPass = @($coldRuns | Where-Object { $_.pass }).Count
$report = [ordered]@{
  schema = 'signal-studio-ss-de-rpo-benchmark-launch-v1'
  configuration = $Configuration
  mode = $Mode
  allowGpu = [bool]$AllowGpu
  gpuExecuted = $launchedGpu
  repeats = $Repeats
  strategy = $Strategy
  rawHeadBudgetMiB = $RawHeadBudgetMiB
  batch = $Batch
  request = $(if ($RequestJson) { [IO.Path]::GetFullPath($RequestJson) } else { $null })
  executableDirectory = $bin
  residentApplication = $false
  warmRelabelledNewProcess = $false
  coldRuns = $coldRuns
  warm = $warm
  baseline = $baseline
  error = $errorText
  formalCommit = '--formal-commit creates a new isolated project inside the benchmark output. The request file and any source project stay untouched.'
  rawHeadArgument = 'not passed; public request keeps retainRawModelOutput false and rawHeadBudgetBytes 0'
  boundary = $(if ($NativeUi) { 'Cold is a new QApplication/window; initial source/chart loading precedes the measured real mouse click. Warm retains that window with excluded warmup. Measurement ends at formal publication/reopen. Missing internal stages remain null; OS cache not flushed; failed runs are excluded from speedups.' } else { 'Cold runs are fresh processes with --formal-commit. Warm is one process with --resident-repeat. Missing internal stages remain null; OS cache not flushed; failed runs are excluded from speedups.' })
  sourceHead = $(if ($SourceRevision) { $SourceRevision } else { & git -C $repo rev-parse HEAD })
  native = [bool]$NativeUi
  osCacheState = 'not flushed; shared OS cache state not controlled'
  modelInitializationState = 'independent Worker and engine/context initialization on every run'
  resourceSamplingBoundary = '200 ms requested polling plus nvidia-smi overhead; GPU memory is whole-device including display; free volume includes other writers; persistent bytes and scratch reservations are separately recorded by the product'
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'launcher.json') -Encoding utf8

if ($errorText) { exit 3 }
if (-not $AllowGpu) { exit 2 }
if ($warm.requested -and $warm.status -ne 'PASS') { exit 2 }
if ($Mode -in @('Cold', 'Both') -and ($coldRuns.Count -ne $Repeats -or $coldPass -ne $Repeats)) { exit 3 }
exit 0
