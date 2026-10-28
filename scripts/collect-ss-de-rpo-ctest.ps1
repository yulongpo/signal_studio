[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
  [Parameter(Mandatory)][string]$FullLog,
  [Parameter(Mandatory)][string[]]$SupplementalLogs,
  [Parameter(Mandatory)][string]$SourceRevision,
  [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$changedRuntime=@(& git -C $repo diff --name-only $SourceRevision -- domain infrastructure application app ui)
if($LASTEXITCODE -ne 0 -or $changedRuntime.Count){throw 'Production sources changed after the full suite; run the full suite again'}
$changedTests=@(& git -C $repo diff --name-only $SourceRevision -- tests)
if($LASTEXITCODE -ne 0 -or @($changedTests | Where-Object {$_ -ne 'tests/ss_de_rpo_ui_tests.cpp'}).Count){throw 'Unrelated tests changed; run the full suite again'}
$tree=Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())"
$registered=(& ctest --test-dir $tree -C $Configuration --show-only=json-v1 | ConvertFrom-Json).tests.name
if($LASTEXITCODE -ne 0 -or -not $registered.Count){throw 'Cannot enumerate current CTest cases'}
$passed=@{}; $failed=@{}; $supplemented=@{}
function Read-Cases([string]$Path,[bool]$Supplement) {
  foreach($line in Get-Content -LiteralPath $Path){
    if($line -match '^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.+\s+(Passed|\*\*\*Failed|\*\*\*Timeout)\b'){
      $name=$Matches[1];$result=$Matches[2]
      if($result -eq 'Passed'){$passed[$name]=$true;if($Supplement){$supplemented[$name]=$true}}
      elseif($Supplement){throw "Supplemental test still fails: $name"}
      else{$failed[$name]=$true}
    }
  }
}
Read-Cases $FullLog $false
$initialPassCount=$passed.Count
foreach($path in $SupplementalLogs){Read-Cases $path $true}
foreach($name in $failed.Keys){if(-not $supplemented.ContainsKey($name)){throw "Full-suite failure was not successfully rerun: $name"}}
foreach($name in $registered){if(-not $passed.ContainsKey($name)){throw "Current CTest case has no passing evidence: $name"}}
if($passed.Count -ne $registered.Count){throw 'Evidence contains a different CTest inventory'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$record=[ordered]@{schema='signal-studio-rpo-ctest-coverage-v1';configuration=$Configuration;pass=$true;
 sourceHead=$SourceRevision;productionSourceEquivalent=$true;changedTests=$changedTests;
 fullLog=(Get-Item -LiteralPath $FullLog).FullName;supplementalLogs=@($SupplementalLogs|ForEach-Object{(Get-Item -LiteralPath $_).FullName});
 initialPassed=$initialPassCount;initialFailures=@($failed.Keys);supplemented=@($supplemented.Keys);
 covered=$passed.Count;registeredTests=@($registered);boundary='Full attempt and directed supplements are distinct runs; this is complete case coverage, not a claim that the first full attempt passed.'}
$record|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $output "$($Configuration.ToLowerInvariant()).json") -Encoding utf8
Write-Host "$Configuration CTest covered=$($passed.Count) initialPassed=$initialPassCount supplemented=$($supplemented.Count)"
