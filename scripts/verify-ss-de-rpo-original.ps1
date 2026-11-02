[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
  [Parameter(Mandatory)][string]$RequestDirectory,
  [Parameter(Mandatory)][string]$OutputDirectory,
  [int[]]$WindowCounts = @(20,159,363,2044),
  [switch]$RawOn
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=[IO.Path]::GetFullPath($OutputDirectory)
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Use a new/empty evidence directory'}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$bin=Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$exe=Join-Path $bin 'SignalStudioDetectionBenchmark.exe'
$rows=@()
foreach($windows in $WindowCounts){
  $retention=if($RawOn){'on'}else{'off'}
  $request=(Get-Item -LiteralPath (Join-Path $RequestDirectory "iq-$windows-raw-$retention.json")).FullName
  $case=Join-Path $output "windows-$windows"
  & $exe --request $request --output $case --formal-commit --strategy sample --batch 2 --timeout 900 *> (Join-Path $output "windows-$windows.process.txt")
  $code=$LASTEXITCODE
  $measurementPath=Join-Path $case 'measurement.json'
  $measurement=if(Test-Path -LiteralPath $measurementPath){Get-Content -LiteralPath $measurementPath -Raw | ConvertFrom-Json}else{$null}
  $pass=$code -eq 0 -and $measurement.pass -and $measurement.formalCommitPerformed -and $measurement.formalProjectReopened -and [int64]$measurement.run.eligibleWindows -eq $windows -and [int64]$measurement.run.completedWindows -eq $windows
  if($RawOn){$pass=$pass -and [int64]$measurement.run.accountedRawHeadBytes -gt 0}
  $rows+=@{windows=$windows;pass=[bool]$pass;exitCode=$code;measurement=$measurementPath;elapsedMs=$measurement.elapsedMs;rawHeadBytes=$measurement.run.accountedRawHeadBytes;sourceHead=(& git -C $repo rev-parse HEAD)}
  $rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'cases.json') -Encoding utf8
  Write-Host "windows=$windows raw=$retention pass=$pass measurement=$measurementPath"
  if(-not $pass){throw "Original-IQ $windows windows failed; preserved $measurementPath"}
}
