[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Configuration='Release',
  [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioDetectionSampleStreamTests.exe"
if(-not(Test-Path -LiteralPath $exe)){throw 'Build/deploy the native Sample tests first'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new/empty evidence directory'}
New-Item -ItemType Directory -Path $output -Force > $null
$scratch=Join-Path $output 'scratch'
New-Item -ItemType Directory -Path $scratch > $null
$report=Join-Path $output 'native-golden.json'
$savedPath=$env:PATH;$process=$null
try {
  $env:PATH="$env:SystemRoot/System32;$env:SystemRoot"
  $arguments=@('--golden','--long','--run-budget','--scratch-root',('"{0}"' -f $scratch),'--golden-report',('"{0}"' -f $report))
  $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path -Parent $exe) -WindowStyle Hidden `
    -PassThru -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt')
  $clock=[Diagnostics.Stopwatch]::StartNew()
  while(-not $process.WaitForExit(200)) {
    if($clock.Elapsed.TotalSeconds -gt 240){$process.Kill();$process.WaitForExit();throw 'Native Sample Golden exceeded 240 seconds'}
  }
  $measurement=if(Test-Path -LiteralPath $report){Get-Content -LiteralPath $report -Raw | ConvertFrom-Json}else{$null}
  $passed=$process.ExitCode -eq 0 -and $measurement.directedTestSuccess
  [ordered]@{schema='signal-studio-sample-native-launch-v1';configuration=$Configuration;executable=$exe;
    executableLastWriteUtc=(Get-Item -LiteralPath $exe).LastWriteTimeUtc.ToString('o');arguments=$arguments;
    exitCode=$process.ExitCode;pass=[bool]$passed;elapsedMs=$clock.ElapsedMilliseconds;systemOnlyPath=$true;
    report=$report;boundary='Native bounded DSP/Legacy Golden and failure cleanup; reduced-Fs long fixtures do not substitute for actual-Fs resource benchmarks.'} |
      ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'launcher.json') -Encoding utf8
  if(-not $passed){throw 'Native Sample checks failed; inspect preserved raw Golden and errors'}
  Write-Host "$Configuration native Sample checks PASS, report=$report"
} finally {
  $env:PATH=$savedPath
  if($process){if(-not $process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
}
