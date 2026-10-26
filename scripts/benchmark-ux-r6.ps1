[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Configuration='Release',
  [Parameter(Mandatory)][string]$RequestJson,
  [Parameter(Mandatory)][string]$OutputDirectory,
  [ValidateSet('sample','roi','coarse','none')][string]$Strategy='roi',
  [ValidateRange(1,16)][int]$Batch=2,
  [ValidateRange(0,2048)][int]$RawHeadBudgetMiB=256,
  [ValidateSet('request','auto','legacy','stream')][string]$SampleEnergyMode='request',
  [ValidateRange(-1,32768)][int]$SampleScratchBudgetMiB=-1,
  [ValidateSet('request','gpu','cpu')][string]$DspCompute='gpu',
  [ValidateRange(1,30)][int]$Repeat=5,
  [double]$LimitSeconds=0,
  [string]$Labels,
  [ValidateRange(0,1000000)][int]$CancelWindows=0,
  [ValidateRange(30,7200)][int]$TimeoutSeconds=900,
  [ValidateSet('Completed','Cancelled','Failed','Rejected')][string]$ExpectedLifecycle='Completed'
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new/empty benchmark output root'}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$request=(Get-Item -LiteralPath $RequestJson).FullName
$exe=Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioDetectionBenchmark.exe"
if(-not(Test-Path -LiteralPath $exe)){throw 'Build/deploy the benchmark and worker first'}
$working=Split-Path -Parent $exe
$monitor=(Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
$savedPath=$env:PATH
$runs=@()
try {
  # Reuse the actual deployed Qt/CRT/model package. No development Qt PATH.
  $env:PATH="$env:SystemRoot/System32;$env:SystemRoot"
  for($index=1;$index -le $Repeat;$index++) {
    $case=Join-Path $output ('run-{0:D2}' -f $index)
    New-Item -ItemType Directory -Path $case | Out-Null
    $native=Join-Path $case 'native'
    $arguments=@('--request',('"{0}"' -f $request),'--output',('"{0}"' -f $native),
        '--strategy',$Strategy,'--batch',"$Batch",'--raw-head-budget-mib',"$RawHeadBudgetMiB",'--timeout',"$TimeoutSeconds")
    if($LimitSeconds -gt 0){$arguments+=@('--limit-seconds',$LimitSeconds.ToString('R',[Globalization.CultureInfo]::InvariantCulture))}
    if($Labels){$arguments+=@('--labels',('"{0}"' -f (Get-Item -LiteralPath $Labels).FullName))}
    if($CancelWindows){$arguments+=@('--cancel-windows',"$CancelWindows")}
    if($DspCompute -ne 'request'){$arguments+=@('--dsp-compute',$DspCompute)}
    if($SampleEnergyMode -ne 'request'){$arguments+=@('--sample-energy-mode',$SampleEnergyMode)}
    if($SampleScratchBudgetMiB -ge 0){$arguments+=@('--sample-scratch-budget-mib',"$SampleScratchBudgetMiB")}
    $gpu=$null;$process=$null;$hardTimeout=$false
    $started=[DateTime]::UtcNow.ToString('o')
    try {
      if($monitor) {
        $gpu=Start-Process -FilePath $monitor -ArgumentList @('--query-gpu=timestamp,index,name,utilization.gpu,utilization.memory,memory.used,memory.total,temperature.gpu,power.draw','--format=csv','--loop-ms=200') `
          -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $case 'gpu.csv') -RedirectStandardError (Join-Path $case 'gpu.stderr.txt')
      }
      $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $working -WindowStyle Hidden -PassThru `
          -RedirectStandardOutput (Join-Path $case 'stdout.json.txt') -RedirectStandardError (Join-Path $case 'stderr.txt')
      $clock=[Diagnostics.Stopwatch]::StartNew()
      while(-not $process.WaitForExit(200)) {
        if($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds+60) {
          $hardTimeout=$true;$process.Kill($true);$process.WaitForExit();break
        }
      }
      $exit=$process.ExitCode
      $path=Join-Path $native 'measurement.json'
      $measurement=if(Test-Path -LiteralPath $path){Get-Content -LiteralPath $path -Raw | ConvertFrom-Json}else{$null}
      $observed=if($measurement.run){$measurement.run.lifecycle}elseif($measurement.error){'Rejected'}else{'Missing'}
      $pass=-not $hardTimeout -and $observed -eq $ExpectedLifecycle -and
          (($observed -eq 'Completed' -and $exit -eq 0 -and $measurement.pass) -or
           ($observed -ne 'Completed' -and $exit -ne 0))
      $runs+=[ordered]@{index=$index;pass=$pass;expectedLifecycle=$ExpectedLifecycle;observedLifecycle=$observed;
        exitCode=$exit;hardTimeout=$hardTimeout;processId=$process.Id;executable=$exe;workingDirectory=$working;
        arguments=$arguments;systemOnlyPath=$true;measurement=$path;gpuCsv=(Join-Path $case 'gpu.csv');
        gpuMonitorAvailable=[bool]$gpu;startedUtc=$started;finishedUtc=[DateTime]::UtcNow.ToString('o');
        gpuLocalUtcOffsetMinutes=[DateTimeOffset]::Now.Offset.TotalMinutes}
      Write-Host "Run $index/$Repeat expected=$ExpectedLifecycle actual=$observed exit=$exit pass=$pass"
    } finally {
      if($process){if(-not $process.HasExited){$process.Kill($true);$process.WaitForExit()};$process.Dispose()}
      if($gpu){if(-not $gpu.HasExited){$gpu.Kill();$gpu.WaitForExit()};$gpu.Dispose()}
    }
  }
} finally {
  $env:PATH=$savedPath
  [ordered]@{schema='signal-studio-ux-r6-benchmark-launch-v1';configuration=$Configuration;repeat=$Repeat;
    request=$request;strategy=$Strategy;requestedBatch=$Batch;limitSeconds=$LimitSeconds;labels=$Labels;
    dspCompute=$DspCompute;sampleEnergyMode=$SampleEnergyMode;sampleScratchBudgetMiB=$SampleScratchBudgetMiB;
    runs=$runs;largeRfbenchExecuted=$false;nativeUiVerified=$false;buildExecuted=$false;
    coldWarmBoundary='fresh isolated worker/model per run; OS caches may be warm; no concurrent benchmark runs'} |
    ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $output 'launcher.json') -Encoding utf8
}
if($runs.Count -ne $Repeat -or @($runs | Where-Object {-not $_.pass}).Count){throw 'Unexpected benchmark outcome; preserve raw reports and diagnose'}
