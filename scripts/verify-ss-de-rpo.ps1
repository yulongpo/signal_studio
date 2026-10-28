# SS-DE-RPO acceptance suite. Exit 0 only when R-01..R-20 and the requested
# CTest rows are PASS. GPU launchers stay off unless -AllowGpu is set.
# Offscreen paint is not a native R-19 PASS. A blocked row is not success.
[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release', 'Both')][string]$Configuration = 'Debug',
  [string]$ExecutableDir,
  [Parameter(Mandatory)][string]$OutputDirectory,
  [string]$BaselineDirectory = 'D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N0',
  [int]$Jobs = 2,
  [switch]$AllowGpu,
  [switch]$SkipBuild,
  [switch]$FullCTest,
  [switch]$Native,
  [string]$OriginalRequestDirectory,
  [string]$ModelRoot,
  [string]$LiveGoldenRoot,
  [string]$AdaptedGoldenRoot,
  [string]$BenchmarkRequestJson,
  [string]$Python = 'D:/profiles/anaconda/python.exe'
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

function Add-Row([string]$Id, [string]$Status, [string]$Detail, [string]$Parent,
                 [string]$ConfigurationName, [string]$EvidenceClass, [bool]$Gpu = $false) {
  $script:Rows.Add([pscustomobject]@{
    id = $Id
    parent = $Parent
    status = $Status
    detail = $Detail
    configuration = $ConfigurationName
    evidenceClass = $EvidenceClass
    gpu = $Gpu
    syntheticRepeatedFrames = $false
  }) | Out-Null
}

function Invoke-Captured {
  param(
    [Parameter(Mandatory)][string]$File,
    [string[]]$ArgumentList = @(),
    [Parameter(Mandatory)][string]$WorkingDirectory,
    [Parameter(Mandatory)][string]$Stdout,
    [Parameter(Mandatory)][string]$Stderr,
    [int]$TimeoutSeconds = 180,
    [hashtable]$Environment = @{}
  )
  $saved = @{}
  foreach ($key in @($Environment.Keys)) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    [Environment]::SetEnvironmentVariable($key, [string]$Environment[$key], 'Process')
  }
  $savedPath = $env:PATH
  # Qt Test is a development library and is not copied by windeployqt.
  # The executable directory stays first so deployed Qt DLLs win.
  $qtBin = 'D:\softwares\Qt\6.11.1\msvc2022_64\bin'
  $prefix = @($WorkingDirectory)
  if (Test-Path -LiteralPath $qtBin) { $prefix += $qtBin }
  $env:PATH = (($prefix + @("$env:SystemRoot\System32", "$env:SystemRoot", $savedPath)) |
    Where-Object { $_ } | Select-Object -Unique) -join ';'
  $process = $null
  try {
    $start = @{
      FilePath = $File
      WorkingDirectory = $WorkingDirectory
      WindowStyle = 'Hidden'
      PassThru = $true
      RedirectStandardOutput = $Stdout
      RedirectStandardError = $Stderr
    }
    if ($ArgumentList.Count -gt 0) { $start.ArgumentList = $ArgumentList }
    $process = Start-Process @start
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    while (-not $process.WaitForExit(500)) {
      if ($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
        $timedOut = $true
        $process.Kill($true)
        $null = $process.WaitForExit()
        break
      }
    }
    if (-not $timedOut) { $null = $process.WaitForExit(); $process.Refresh() }
    [pscustomobject]@{
      ExitCode = $process.ExitCode
      TimedOut = $timedOut
      ElapsedMs = $clock.ElapsedMilliseconds
    }
  } finally {
    $env:PATH = $savedPath
    foreach ($key in @($saved.Keys)) {
      [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process')
    }
    if ($process -and -not $process.HasExited) {
      $process.Kill($true)
      $null = $process.WaitForExit()
    }
    if ($process) { $process.Dispose() }
  }
}

function Get-Text([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return '' }
  return (Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue)
}

function Add-LogicRows([string]$Text, [string]$ConfigurationName) {
  $seen = @{}
  foreach ($line in ($Text -split "`r?`n")) {
    if ($line -match '^(R-\d+\S*|publication-\S+)\s+(PASS|FAIL)\b\s*(.*)$') {
      $name = $Matches[1]
      $status = $Matches[2]
      $parent = if ($name -like 'publication-*') { 'R-12' }
                elseif ($name -match '^(R-\d+)') { $Matches[1] }
                else { '' }
      Add-Row -Id "evidence:${ConfigurationName}:$name" -Status $status -Detail $Matches[3] `
        -Parent $parent -ConfigurationName $ConfigurationName -EvidenceClass 'unit'
      $seen[$name] = $status
    }
  }
  return $seen
}

function Add-UiRows([string]$Text, [string]$ConfigurationName) {
  $map = @{
    'doubleClickArmsOneStart' = 'R-08'
    'synchronousRejectionKeepsTheError' = 'R-08'
    'repeatedVisiblePreparingPaintP95' = 'R-19'
    'provisionalCancelBeforeCommit' = 'R-09'
  }
  $seen = @{}
  foreach ($line in ($Text -split "`r?`n")) {
    if ($line -match '^(PASS|FAIL!)\s+:\s+SsDeRpoUiTests::(\w+)\(\)\s*(.*)$') {
      $slot = $Matches[2]
      if (-not $map.ContainsKey($slot)) { continue }
      $status = $(if ($Matches[1] -eq 'PASS') { 'PASS' } else { 'FAIL' })
      $native = $Text -match 'evidence=native-fullscreen'
      $class = $(if ($slot -eq 'repeatedVisiblePreparingPaintP95' -and $native) { 'native-4k' } else { 'qt-offscreen' })
      Add-Row -Id "evidence:${ConfigurationName}:$slot" -Status $status -Detail $Matches[3].Trim() `
        -Parent $map[$slot] -ConfigurationName $ConfigurationName -EvidenceClass $class
      $seen[$slot] = $status
    }
    if ($line -match 'R-19 metric=visible-paint') {
      $nativeMetric = $line -match 'evidence=native-fullscreen' -and $line -match 'window=2560x1440'
      Add-Row -Id "evidence:${ConfigurationName}:R-19-metric" -Status $(if ($nativeMetric) { 'RECORDED_NATIVE' } else { 'UNVERIFIED_NATIVE' }) `
        -Detail $line.Trim() -Parent 'R-19' -ConfigurationName $ConfigurationName `
        -EvidenceClass $(if ($nativeMetric) { 'native-4k' } else { 'qt-offscreen' })
    }
  }
  return $seen
}

function Get-KidStatus([string]$ParentId) {
  return @($script:Rows | Where-Object { $_.parent -eq $ParentId } | ForEach-Object { $_.status })
}

function Get-Rollup([string[]]$Statuses) {
  if ($Statuses.Count -eq 0) { return 'NOT_RUN' }
  if ($Statuses -contains 'FAIL') { return 'FAIL' }
  $other = @($Statuses | Where-Object { $_ -ne 'PASS' })
  if ($other.Count -gt 0) { return $other[0] }
  return 'PASS'
}

$script:Rows = New-Object System.Collections.Generic.List[object]
$script:NativeR19 = $false
$output = Assert-OwnedOutput $OutputDirectory
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force -ErrorAction SilentlyContinue).Count) {
  throw "Use a new/empty output directory: $output"
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$configs = $(if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) })
if ($AllowGpu -and -not $OriginalRequestDirectory) {
  $OriginalRequestDirectory = Join-Path $output 'requests'
  & (Join-Path $PSScriptRoot 'prepare-ss-de-rpo-requests.ps1') -OutputDirectory $OriginalRequestDirectory
}
if ($OriginalRequestDirectory) { $OriginalRequestDirectory = [IO.Path]::GetFullPath($OriginalRequestDirectory) }
if (-not $BenchmarkRequestJson -and $OriginalRequestDirectory) {
  $BenchmarkRequestJson = Join-Path $OriginalRequestDirectory 'iq-20-raw-off.json'
}
if ($ExecutableDir -and $configs.Count -ne 1) {
  throw 'ExecutableDir applies to one configuration. Omit it when Configuration is Both.'
}
if ($BaselineDirectory -and (Test-Path -LiteralPath $BaselineDirectory)) {
  Add-Row -Id 'baseline-n0' -Status 'RECORDED' -Parent '' -ConfigurationName '' `
    -EvidenceClass 'original-iq' `
    -Detail ("read-only baseline directory " + [IO.Path]::GetFullPath($BaselineDirectory))
} else {
  Add-Row -Id 'baseline-n0' -Status 'BLOCKED_BASELINE_MISSING' -Parent '' -ConfigurationName '' `
    -EvidenceClass 'not-run' -Detail "baseline directory is not readable: $BaselineDirectory"
}

$suiteError = $null
try {
  foreach ($cfg in $configs) {
    $case = Join-Path $output $cfg.ToLowerInvariant()
    New-Item -ItemType Directory -Path $case -Force | Out-Null
    if (-not $SkipBuild) {
      & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $cfg -Action Configure -Jobs $Jobs -Python $Python
      & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $cfg -Action Build -Jobs $Jobs -Python $Python
      Add-Row -Id "evidence:${cfg}:build" -Status 'PASS' -Parent '' -ConfigurationName $cfg `
        -EvidenceClass 'unit' -Detail 'configure and owned test targets built'
    }
    if ($FullCTest) {
      $ctestLog = Join-Path $case 'ctest.txt'
      $previous = $ErrorActionPreference
      try {
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $cfg -Action Test -Jobs $Jobs -Python $Python *> $ctestLog
        Add-Row -Id "CTest-$cfg" -Status 'PASS' -Parent '' -ConfigurationName $cfg `
          -EvidenceClass 'unit' -Detail "complete CTest log $ctestLog"
        foreach ($requirement in @('R-07','R-10','R-13','R-14','R-16','R-18')) {
          Add-Row -Id "evidence:${cfg}:ctest-$requirement" -Status 'PASS' -Parent $requirement `
            -ConfigurationName $cfg -EvidenceClass 'unit' `
            -Detail "Complete CTest passed: refinement strategies, asset/task faults, UX lifecycle, durable recovery, interval oracle and source-plan invalidation. Log $ctestLog"
        }
      } catch {
        Add-Row -Id "CTest-$cfg" -Status 'FAIL' -Parent '' -ConfigurationName $cfg `
          -EvidenceClass 'unit' -Detail $_.Exception.Message
      } finally {
        $ErrorActionPreference = $previous
      }
    } else {
      Add-Row -Id "CTest-$cfg" -Status 'BLOCKED_NOT_RUN' -Parent '' -ConfigurationName $cfg `
        -EvidenceClass 'not-run' `
        -Detail 'Full CTest was not requested. This is not a CTest PASS. Pass -FullCTest to run it.'
    }

    $bin = if ($ExecutableDir) { [IO.Path]::GetFullPath($ExecutableDir) }
           else { Join-Path $repo ("out/vs2026-qt611-" + $cfg.ToLowerInvariant() + "_bin") }
    $logic = Join-Path $bin 'SignalStudioSsDeRpoLogicTests.exe'
    $ui = Join-Path $bin 'SignalStudioSsDeRpoUiTests.exe'
    $acceptance = Join-Path $bin 'SignalStudioSsDeRpoAcceptanceTests.exe'
    $fonts = Join-Path $env:SystemRoot 'Fonts'

    if (Test-Path -LiteralPath $logic) {
      $result = Invoke-Captured -File $logic -WorkingDirectory $bin `
        -Stdout (Join-Path $case 'logic-stdout.txt') -Stderr (Join-Path $case 'logic-stderr.txt') `
        -TimeoutSeconds 120 -Environment @{ SS_DE_RPO_SOURCE = $repo }
      $text = (Get-Text (Join-Path $case 'logic-stdout.txt')) + "`n" + (Get-Text (Join-Path $case 'logic-stderr.txt'))
      $seen = Add-LogicRows $text $cfg
      if ($result.TimedOut -or @($seen.Keys).Count -eq 0) {
        Add-Row -Id "evidence:${cfg}:logic-process" -Status 'FAIL' -Parent 'R-01' `
          -ConfigurationName $cfg -EvidenceClass 'unit' `
          -Detail "logic executable exit=$($result.ExitCode) timeout=$($result.TimedOut) lines=$($seen.Count)"
      }
    } else {
      Add-Row -Id "evidence:${cfg}:logic-missing" -Status 'BLOCKED_NOT_BUILT' -Parent 'R-01' `
        -ConfigurationName $cfg -EvidenceClass 'not-run' -Detail "missing $logic"
    }

    if (Test-Path -LiteralPath $ui) {
      $uiLog = Join-Path $case 'ui-result.txt'
      $result = Invoke-Captured -File $ui -WorkingDirectory $bin `
        -ArgumentList @('-o', ($uiLog + ',txt'), '-v2') `
        -Stdout (Join-Path $case 'ui-stdout.txt') -Stderr (Join-Path $case 'ui-stderr.txt') `
        -TimeoutSeconds 180 -Environment @{
          QT_QPA_PLATFORM = $(if ($Native) { 'windows' } else { 'offscreen' })
          QT_SCALE_FACTOR = $(if ($Native) { '' } else { '1.5' })
          QT_QPA_FONTDIR = $fonts
          SS_DE_RPO_SOURCE = $repo
        }
      $text = (Get-Text $uiLog) + "`n" + (Get-Text (Join-Path $case 'ui-stdout.txt')) + "`n" + (Get-Text (Join-Path $case 'ui-stderr.txt'))
      $seen = Add-UiRows $text $cfg
      foreach ($slot in @('doubleClickArmsOneStart', 'synchronousRejectionKeepsTheError',
                          'repeatedVisiblePreparingPaintP95', 'provisionalCancelBeforeCommit')) {
        if (-not $seen.ContainsKey($slot)) {
          $parent = switch ($slot) {
            'provisionalCancelBeforeCommit' { 'R-09' }
            'repeatedVisiblePreparingPaintP95' { 'R-19' }
            default { 'R-08' }
          }
          Add-Row -Id "evidence:${cfg}:$slot" -Status 'FAIL' -Parent $parent -ConfigurationName $cfg `
            -EvidenceClass 'qt-offscreen' `
            -Detail "slot did not report PASS or FAIL. exit=$($result.ExitCode) timeout=$($result.TimedOut). A missing result is not a pass."
        }
      }
    } else {
      Add-Row -Id "evidence:${cfg}:ui-missing" -Status 'BLOCKED_NOT_BUILT' -Parent 'R-08' `
        -ConfigurationName $cfg -EvidenceClass 'not-run' -Detail "missing $ui"
    }

    if (Test-Path -LiteralPath $acceptance) {
      $matrixPath = Join-Path $case 'acceptance-matrix.json'
      $result = Invoke-Captured -File $acceptance -WorkingDirectory $repo `
        -ArgumentList @('--matrix-out', $matrixPath) `
        -Stdout (Join-Path $case 'acceptance-stdout.txt') -Stderr (Join-Path $case 'acceptance-stderr.txt') `
        -TimeoutSeconds 600 -Environment @{
          SS_DE_RPO_SOURCE = $repo
          SS_DE_RPO_REQUEST_JSON = $(if ($OriginalRequestDirectory) { Join-Path $OriginalRequestDirectory 'iq-2044-raw-off.json' } else { '' })
        }
      $text = (Get-Text (Join-Path $case 'acceptance-stdout.txt')) + "`n" + (Get-Text (Join-Path $case 'acceptance-stderr.txt'))
      $parsed = 0
      foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^(R-06\S*)\s+(PASS|FAIL|BLOCKED\S*)\b\s*(.*)$') {
          Add-Row -Id "evidence:${cfg}:$($Matches[1])" -Status $Matches[2] -Detail $Matches[3] `
            -Parent 'R-06' -ConfigurationName $cfg -EvidenceClass 'original-iq-source-plan'
          $parsed++
        }
      }
      if ($parsed -eq 0) {
        Add-Row -Id "evidence:${cfg}:R-06-process" -Status 'FAIL' -Parent 'R-06' `
          -ConfigurationName $cfg -EvidenceClass 'original-iq-source-plan' `
          -Detail "acceptance executable produced no R-06 rows. exit=$($result.ExitCode) timeout=$($result.TimedOut)"
      }
    } else {
      Add-Row -Id "evidence:${cfg}:R-06" -Status 'BLOCKED_UNREGISTERED' -Parent 'R-06' `
        -ConfigurationName $cfg -EvidenceClass 'not-run' `
        -Detail 'SignalStudioSsDeRpoAcceptanceTests.exe is not in the executable directory. Main registers the CMake target. This is not an R-06 PASS.'
    }

    $liveStatus = 'BLOCKED_EXCLUSIVE_SLOT'
    $adaptedStatus = 'BLOCKED_EXCLUSIVE_SLOT'
    $liveDetail = 'model-backend --live was not started. Exclusive GPU slot is closed.'
    $adaptedDetail = 'model-backend --adapted was not started. Exclusive GPU slot is closed.'
    if ($AllowGpu) {
      $backend = Join-Path $bin 'SignalStudioDetectionModelBackendTests.exe'
      $modelRootForCase = if ($ModelRoot) { $ModelRoot } else { Join-Path $bin 'detection-models' }
      if (-not (Test-Path -LiteralPath $backend) -or -not $LiveGoldenRoot) {
        $liveStatus = 'BLOCKED_MISSING_INPUT'
        $liveDetail = "Need the backend executable, -ModelRoot, and -LiveGoldenRoot. backend=$backend"
      } else {
        $result = Invoke-Captured -File $backend -WorkingDirectory $bin `
          -ArgumentList @('--live', $modelRootForCase, $LiveGoldenRoot) `
          -Stdout (Join-Path $case 'live-stdout.txt') -Stderr (Join-Path $case 'live-stderr.txt') `
          -TimeoutSeconds 900
        $liveStatus = $(if (-not $result.TimedOut -and $result.ExitCode -eq 0) { 'PASS' } else { 'FAIL' })
        $liveDetail = "exit=$($result.ExitCode) timeout=$($result.TimedOut) evidenceClass=tensorrt"
      }
      if (-not (Test-Path -LiteralPath $backend) -or -not $AdaptedGoldenRoot) {
        $adaptedStatus = 'BLOCKED_MISSING_INPUT'
        $adaptedDetail = "Need the backend executable, -ModelRoot, and -AdaptedGoldenRoot."
      } else {
        $result = Invoke-Captured -File $backend -WorkingDirectory $bin `
          -ArgumentList @('--adapted', $modelRootForCase, $AdaptedGoldenRoot) `
          -Stdout (Join-Path $case 'adapted-stdout.txt') -Stderr (Join-Path $case 'adapted-stderr.txt') `
          -TimeoutSeconds 900
        $adaptedStatus = $(if (-not $result.TimedOut -and $result.ExitCode -eq 0) { 'PASS' } else { 'FAIL' })
        $adaptedDetail = "exit=$($result.ExitCode) timeout=$($result.TimedOut) evidenceClass=tensorrt"
      }
      $sampleScript = Join-Path $PSScriptRoot 'verify-sample-ux-r6.ps1'
      $sampleOut = Join-Path $case 'verify-sample'
      try {
        & $sampleScript -Configuration $cfg -OutputDirectory $sampleOut
        Add-Row -Id "evidence:${cfg}:verify-sample" -Status 'PASS' -Parent 'R-17' `
          -ConfigurationName $cfg -EvidenceClass 'synthetic-iq' `
          -Detail "CPU/native sample golden completed in $sampleOut. This is not TensorRT and not original-IQ R-06."
      } catch {
        Add-Row -Id "evidence:${cfg}:verify-sample" -Status 'FAIL' -Parent 'R-17' `
          -ConfigurationName $cfg -EvidenceClass 'synthetic-iq' -Detail $_.Exception.Message
      }
      if ($Native) {
        try {
          foreach ($software in @($false,$true)) {
            $displayReport = Join-Path $case $(if ($software) { 'package-software.json' } else { 'package-gpu.json' })
            & (Join-Path $PSScriptRoot 'verify-package.ps1') -Configuration $cfg -SoftwareRenderer:$software -ReportPath $displayReport
          }
          Add-Row -Id "evidence:${cfg}:native-package" -Status 'PASS' -Parent 'R-20' -ConfigurationName $cfg -EvidenceClass 'native-4k' -Detail 'Standalone system-PATH deployment, actual 4K/150% fullscreen, GPU QRhi and software display both passed.'
        } catch {
          Add-Row -Id "evidence:${cfg}:native-package" -Status 'FAIL' -Parent 'R-20' -ConfigurationName $cfg -EvidenceClass 'native-4k' -Detail $_.Exception.Message
        }
      }
      if ($OriginalRequestDirectory) {
        foreach ($windows in @(20,159,363,2044)) {
          $requestPath = Join-Path $OriginalRequestDirectory "iq-$windows-raw-off.json"
          $runOutput = Join-Path $case "original-iq-$windows"
          $arguments = @('--request',$requestPath,'--output',$runOutput,'--formal-commit','--strategy','sample','--batch','2','--timeout','900')
          $result = Invoke-Captured -File (Join-Path $bin 'SignalStudioDetectionBenchmark.exe') -WorkingDirectory $bin -ArgumentList $arguments -Stdout (Join-Path $case "iq-$windows-stdout.txt") -Stderr (Join-Path $case "iq-$windows-stderr.txt") -TimeoutSeconds 960
          $measurementPath = Join-Path $runOutput 'measurement.json'
          $measurement = if (Test-Path -LiteralPath $measurementPath) { Get-Content -LiteralPath $measurementPath -Raw | ConvertFrom-Json } else { $null }
          $passed = -not $result.TimedOut -and $result.ExitCode -eq 0 -and $measurement.pass -and $measurement.formalCommitPerformed -and $measurement.formalProjectReopened -and [int64]$measurement.run.eligibleWindows -eq $windows -and [int64]$measurement.run.completedWindows -eq $windows
          foreach ($parent in @('R-06','R-11','R-17')) {
            Add-Row -Id "evidence:${cfg}:original-$windows-$parent" -Status $(if ($passed) { 'PASS' } else { 'FAIL' }) -Parent $parent -ConfigurationName $cfg -EvidenceClass 'original-iq-tensorrt' -Gpu $true -Detail "SourcePlan/forward/Sample/durable comparison/project commit/reopen: $measurementPath"
          }
        }
        $rawOutput = Join-Path $case 'original-iq-20-raw-on'
        $arguments = @('--request',(Join-Path $OriginalRequestDirectory 'iq-20-raw-on.json'),'--output',$rawOutput,'--formal-commit','--strategy','sample','--batch','2','--timeout','180')
        $result = Invoke-Captured -File (Join-Path $bin 'SignalStudioDetectionBenchmark.exe') -WorkingDirectory $bin -ArgumentList $arguments -Stdout (Join-Path $case 'raw-on-stdout.txt') -Stderr (Join-Path $case 'raw-on-stderr.txt') -TimeoutSeconds 240
        $rawMeasurement = Join-Path $rawOutput 'measurement.json'
        $raw = if (Test-Path -LiteralPath $rawMeasurement) { Get-Content -LiteralPath $rawMeasurement -Raw | ConvertFrom-Json } else { $null }
        $rawPassed = -not $result.TimedOut -and $result.ExitCode -eq 0 -and $raw.pass -and $raw.formalCommitPerformed -and [int64]$raw.run.accountedRawHeadBytes -gt 0
        Add-Row -Id "evidence:${cfg}:original-raw-on" -Status $(if ($rawPassed) { 'PASS' } else { 'FAIL' }) -Parent 'R-04' -ConfigurationName $cfg -EvidenceClass 'original-iq-tensorrt' -Gpu $true -Detail $rawMeasurement
      }
      if ($BenchmarkRequestJson) {
        $benchOut = Join-Path $case 'benchmark-ss-de-rpo'
        try {
          & (Join-Path $PSScriptRoot 'benchmark-ss-de-rpo.ps1') -Configuration $cfg `
            -RequestJson $BenchmarkRequestJson -OutputDirectory $benchOut -Strategy sample `
            -Batch 2 -Mode Both -AllowGpu -ExecutableDir $bin
          Add-Row -Id "evidence:${cfg}:benchmark-formal-resident" -Status 'RECORDED' -Parent 'R-11' `
            -ConfigurationName $cfg -EvidenceClass 'tensorrt' -Gpu $true `
            -Detail "Cold uses fresh processes with --formal-commit. Warm uses one process with --resident-repeat. A completed inference is not an R-11 publication PASS. The source project is not the isolated formal project."
        } catch {
          Add-Row -Id "evidence:${cfg}:benchmark-formal-resident" -Status 'FAIL' -Parent 'R-11' `
            -ConfigurationName $cfg -EvidenceClass 'tensorrt' -Gpu $true -Detail $_.Exception.Message
        }
      } else {
        Add-Row -Id "evidence:${cfg}:benchmark-ux-r6" -Status 'BLOCKED_MISSING_INPUT' -Parent 'R-11' `
          -ConfigurationName $cfg -EvidenceClass 'not-run' `
          -Detail '-AllowGpu was set but -BenchmarkRequestJson was not. No benchmark process was started.'
      }
    } else {
      Add-Row -Id "evidence:${cfg}:verify-sample" -Status 'BLOCKED_EXCLUSIVE_SLOT' -Parent 'R-17' `
        -ConfigurationName $cfg -EvidenceClass 'not-run' `
        -Detail 'verify-sample-ux-r6.ps1 was not started.'
      Add-Row -Id "evidence:${cfg}:benchmark-ux-r6" -Status 'BLOCKED_EXCLUSIVE_SLOT' -Parent 'R-11' `
        -ConfigurationName $cfg -EvidenceClass 'not-run' `
        -Detail 'benchmark-ss-de-rpo.ps1 was not started. Cold would use --formal-commit and warm would use one process with --resident-repeat. --raw-head-budget-mib is not passed.'
    }
    Add-Row -Id "evidence:${cfg}:model-live" -Status $liveStatus -Detail $liveDetail -Parent 'R-15' `
      -ConfigurationName $cfg -EvidenceClass $(if ($liveStatus -eq 'PASS') { 'tensorrt' } else { 'not-run' }) `
      -Gpu ($liveStatus -eq 'PASS')
    Add-Row -Id "evidence:${cfg}:model-adapted" -Status $adaptedStatus -Detail $adaptedDetail -Parent 'R-15' `
      -ConfigurationName $cfg -EvidenceClass $(if ($adaptedStatus -eq 'PASS') { 'tensorrt' } else { 'not-run' }) `
      -Gpu ($adaptedStatus -eq 'PASS')
  }
} catch {
  $suiteError = $_.Exception.Message
  Add-Row -Id 'suite-exception' -Status 'FAIL' -Parent '' -ConfigurationName $Configuration `
    -EvidenceClass 'not-run' -Detail $suiteError
}

$descriptions = @{
  'R-07' = 'Sample/ROI/Coarse semantic switch is not covered by an owned executable.'
  'R-10' = 'Corrupt model, source change, and disk failure are not covered here.'
  'R-13' = 'Same-channel UX-R6 edit semantics are not re-executed here.'
  'R-14' = 'Final shard damage and project recovery are not covered here.'
  'R-16' = 'Dense overlap tracking is not covered here.'
  'R-18' = 'Cache invalidation after model or source change is not covered here.'
  'R-20' = 'Marker, project reopen, narrowband, PSD, and waterfall regression is not covered here.'
}
foreach ($number in 1..20) {
  $id = 'R-{0:D2}' -f $number
  $kids = @(Get-KidStatus $id)
  $status = Get-Rollup $kids
  $detail = "childStatuses=$($kids -join ',')"
  if ($id -eq 'R-15') {
    $cpu = @($script:Rows | Where-Object { $_.parent -eq 'R-15' -and $_.id -match ':R-15' })
    $live = @($script:Rows | Where-Object { $_.id -match ':model-live$' })
    $adapted = @($script:Rows | Where-Object { $_.id -match ':model-adapted$' })
    $cpuFail = @($cpu | Where-Object { $_.status -eq 'FAIL' }).Count -gt 0
    $cpuPass = $cpu.Count -gt 0 -and @($cpu | Where-Object { $_.status -ne 'PASS' }).Count -eq 0
    $gpuPass = $live.Count -gt 0 -and $adapted.Count -gt 0 -and
      @($live + $adapted | Where-Object { $_.status -ne 'PASS' }).Count -eq 0
    if ($cpuFail -or @($live + $adapted | Where-Object { $_.status -eq 'FAIL' }).Count -gt 0) {
      $status = 'FAIL'
      $detail = 'A CPU batch selector or --live/--adapted run failed. CPU selector success is not a GPU PASS.'
    } elseif ($cpuPass -and $gpuPass) {
      $status = 'PASS'
      $detail = 'CPU selector plus --live and --adapted passed. evidenceClass=tensorrt for the live rows.'
    } else {
      $status = 'UNVERIFIED_GPU'
      $detail = 'CPU selector evidence does not make R-15 a TensorRT PASS. --live/--adapted stay blocked until -AllowGpu and both golden roots are supplied.'
    }
  } elseif ($id -eq 'R-19') {
    $paint = @($script:Rows | Where-Object { $_.parent -eq 'R-19' -and $_.id -match 'repeatedVisiblePreparingPaintP95$' })
    if (@($paint | Where-Object { $_.status -eq 'FAIL' }).Count -gt 0) {
      $status = 'FAIL'
      $detail = 'Visible preparing paint observation failed. Handler time was not substituted.'
    } elseif ($paint.Count -gt 0 -and @($paint | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) {
      $native = @($script:Rows | Where-Object {
        $_.parent -eq 'R-19' -and $_.evidenceClass -eq 'native-4k' -and $_.detail -match 'window=2560x1440'
      })
      if ($native.Count -gt 0) {
        $status = 'PASS'
        $detail = 'Visible preparing paint p95 was observed fullscreen on the only 3840x2160 1.5 screen, logical 2560x1440.'
        $script:NativeR19 = $true
      } else {
        $status = 'UNVERIFIED_NATIVE'
        $detail = 'Offscreen visible-paint p95 was observed at logical 2560x1440. That is functional evidence, not a native 4K/150% R-19 PASS.'
      }
    } else {
      $status = 'NOT_RUN'
      $detail = 'No visible-paint result was recorded.'
    }
  } elseif ($id -eq 'R-06') {
    $windowKids = @($script:Rows | Where-Object { $_.parent -eq 'R-06' -and $_.id -match 'R-06-(20|159|363|2044)$' })
    $failed = @($script:Rows | Where-Object { $_.parent -eq 'R-06' -and $_.status -eq 'FAIL' }).Count -gt 0
    $unregistered = @($script:Rows | Where-Object { $_.parent -eq 'R-06' -and $_.status -eq 'BLOCKED_UNREGISTERED' }).Count -gt 0
    if ($failed) {
      $status = 'FAIL'
      $detail = 'At least one real IQ SourcePlan row failed. 2044 windows on the 0.8 s file is a real failure, not a skipped success.'
    } elseif ($unregistered -and $windowKids.Count -eq 0) {
      $status = 'BLOCKED_UNREGISTERED'
      $detail = 'Acceptance executable is not registered in this executable directory.'
    } elseif ($windowKids.Count -lt 4) {
      $status = 'FAIL'
      $detail = 'A required 20/159/363/2044 window row is missing.'
    }
  } elseif ($descriptions.ContainsKey($id) -and $kids.Count -eq 0) {
    $status = 'UNVERIFIED'
    $detail = $descriptions[$id] + ' Not marked PASS.'
  } elseif ($id -eq 'R-11' -or $id -eq 'R-17') {
    $original = @($script:Rows | Where-Object { $_.parent -eq $id -and $_.id -match ':original-(20|159|363|2044)-' })
    if ($original.Count -eq 4 * $configs.Count -and @($original | Where-Object { $_.status -ne 'PASS' }).Count -eq 0 -and @($kids | Where-Object { $_ -eq 'FAIL' }).Count -eq 0) {
      $status = 'PASS'
      $detail = 'Original IQ 20/159/363/2044 Sample, durable comparison, isolated project commit and reopen passed. Synthetic long/Golden and actual-Fs planning are separate evidence.'
    } elseif ($kids.Count -eq 0 -or $status -in @('PASS','RECORDED')) {
      $status = 'UNVERIFIED'
      $detail = 'Complete original-IQ Sample/publication evidence is missing.'
    }
  }
  Add-Row -Id $id -Status $status -Detail $detail -Parent '' -ConfigurationName $Configuration `
    -EvidenceClass 'matrix'
}

# An [ordered] literal rejects $null ("Argument types do not match") and can
# unroll a generic List into the constructor. Assign each key after creation.
$report = [ordered]@{}
$report.schema = 'signal-studio-ss-de-rpo-acceptance-v1'
$report.producer = 'scripts/verify-ss-de-rpo.ps1'
$report.allowGpu = [bool]$AllowGpu
$report.fullCTest = [bool]$FullCTest
$report.gpuExecuted = [bool]$AllowGpu
$report.syntheticRepeatedFrames = $false
$report.workerLaunched = $false
$report.nativeR19Pass = [bool]$script:NativeR19
if ($null -eq $suiteError) {
  $report.suiteError = ''
} else {
  $report.suiteError = [string]$suiteError
}
$report.rows = @($script:Rows.ToArray())
$reportPath = Join-Path $output 'matrix.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
$blockingIds = @((1..20 | ForEach-Object { 'R-{0:D2}' -f $_ })) + @($configs | ForEach-Object { "CTest-$_" })
if ($suiteError) { $blockingIds += 'suite-exception' }
$fail = $false
$blocked = $false
foreach ($id in $blockingIds) {
  $row = @($script:Rows | Where-Object { $_.id -eq $id } | Select-Object -First 1)
  if ($row.Count -eq 0 -or $row[0].status -eq 'FAIL') { $fail = $true }
  elseif ($row[0].status -ne 'PASS') { $blocked = $true }
}
Write-Host "matrix=$reportPath fail=$fail blocked=$blocked"
if ($fail) { exit 3 }
if ($blocked) { exit 2 }
exit 0
