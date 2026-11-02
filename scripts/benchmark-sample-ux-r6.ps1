[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Configuration='Release',
  [Parameter(Mandatory)][string]$OutputDirectory,
  [double[]]$Durations=@(0.1,1,10,60),
  [ValidateRange(1,1e12)][double]$SampleRate=20000000,
  [ValidateSet('continuous','bursts','blank','low-snr','overlap','hopping')][string[]]$Scenarios=@('continuous','bursts'),
  [ValidateSet('legacy','stream','auto')][string[]]$Modes=@('legacy','stream'),
  [ValidateSet('cpu','gpu')][string]$DspCompute='cpu',
  [ValidateRange(1,10)][int]$Repeat=1,
  [ValidateRange(16,1024)][int]$MemoryMiB=96,
  [ValidateRange(0,32768)][int]$ScratchMiB=16384,
  [ValidateRange(1,131072)][int]$BlockSamples=131072,
  [ValidateRange(30,7200)][int]$TimeoutSeconds=1800
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioSampleRefinementBenchmark.exe"
if(-not(Test-Path -LiteralPath $exe)){throw 'Build and deploy Sample benchmark first'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new/empty output directory'}
foreach($duration in $Durations){if(-not [double]::IsFinite($duration) -or $duration -le 0){throw 'Invalid physical duration'}}
New-Item -ItemType Directory -Path $output -Force > $null
$savedPath=$env:PATH
$runs=@()
try {
  $env:PATH="$env:SystemRoot/System32;$env:SystemRoot"
  foreach($scenario in $Scenarios){foreach($duration in $Durations){foreach($mode in $Modes){
    for($index=1;$index -le $Repeat;$index++) {
      $durationText=$duration.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
      $name="$scenario-$durationText-$mode-$('{0:D2}' -f $index)"
      $case=Join-Path $output $name;New-Item -ItemType Directory -Path $case > $null
      $native=Join-Path $case 'native'
      $arguments=@('--dsp-compute',$DspCompute,'--output',('"{0}"' -f $native),'--seconds',$durationText,
        '--sample-rate',$SampleRate.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
        '--scenario',$scenario,'--mode',$mode,'--memory-mib',"$MemoryMiB",'--scratch-mib',"$ScratchMiB",
        '--block',"$BlockSamples",'--timeout',"$TimeoutSeconds")
      $process=$null;$hardTimeout=$false;$started=[DateTime]::UtcNow.ToString('o')
      try {
        $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path -Parent $exe) `
          -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $case 'stdout.txt') -RedirectStandardError (Join-Path $case 'stderr.txt')
        $clock=[Diagnostics.Stopwatch]::StartNew()
        while(-not $process.WaitForExit(1000)) {
          $process.Refresh()
          [pscustomobject]@{elapsedMs=$clock.ElapsedMilliseconds;cpuMs=$process.TotalProcessorTime.TotalMilliseconds;
            rssBytes=$process.WorkingSet64;peakRssBytes=$process.PeakWorkingSet64;privateBytes=$process.PrivateMemorySize64;
            virtualBytes=$process.VirtualMemorySize64} | Export-Csv -LiteralPath (Join-Path $case 'resource-samples.csv') -Append -NoTypeInformation -Encoding utf8
          if($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds+60){$hardTimeout=$true;$process.Kill();$process.WaitForExit();break}
        }
        $measurementPath=Join-Path $native 'measurement.json'
        $m=if(Test-Path -LiteralPath $measurementPath){Get-Content -LiteralPath $measurementPath -Raw | ConvertFrom-Json}else{$null}
        $outcome=if($hardTimeout){'HardTimeout'}elseif(-not $m){'Missing'}
          elseif($m.helperReturned -and $m.pass -and $process.ExitCode -eq 0){'Returned'}
          elseif(-not $m.helperReturned -and $m.failureCode -eq 2 -and $process.ExitCode -ne 0){'ResourceLimited'}else{'Failed'}
        # A bounded Legacy refusal is retained with native pass=false/exit=1;
        # it is never counted as a completed baseline or a speedup denominator.
        $runs+=[ordered]@{case=$name;scenario=$scenario;seconds=$duration;mode=$mode;index=$index;
          outcome=$outcome;exitCode=$process.ExitCode;hardTimeout=$hardTimeout;measurement=$measurementPath;
          processId=$process.Id;executable=$exe;arguments=$arguments;startedUtc=$started;
          finishedUtc=[DateTime]::UtcNow.ToString('o');systemOnlyPath=$true}
        Write-Host "$name outcome=$outcome exit=$($process.ExitCode) ms=$($m.elapsedMs) eligible=$($m.publicationEligible) error=$($m.error)"
      } finally {
        if($process){if(-not $process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
      }
    }
  }}}
} finally {
  $env:PATH=$savedPath
  [ordered]@{schema='signal-studio-sample-benchmark-launch-v1';configuration=$Configuration;runs=$runs;
    dspCompute=$DspCompute;sampleRateHz=$SampleRate;memoryMiB=$MemoryMiB;scratchMiB=$ScratchMiB;outputBlockSamples=$BlockSamples;
    boundary='Synthetic stateless IQ / actual requested Fs. Per-mode fresh process. Resource refusals are not completed baselines. Actual DSP backend recorded separately; no model/UI/field-RF claim.'} |
      ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $output 'launcher.json') -Encoding utf8
}
if(@($runs | Where-Object {$_.outcome -in @('HardTimeout','Missing','Failed')}).Count){throw 'Failed/missing/timeout Sample measurements; inspect preserved evidence'}
