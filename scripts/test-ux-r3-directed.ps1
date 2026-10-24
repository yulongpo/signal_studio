[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug',
      [ValidateSet('Workflows','Axes','Narrowband','Ui','Ux')][string]$Suite='Workflows',
      [string[]]$Cases=@(),[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repoRoot 'out/tasks/wideband-detection/UX-R3/G7/diagnostics'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$target=if($Suite -eq 'Workflows'){'SignalStudioUxR3WorkflowTests'}elseif($Suite -eq 'Axes'){'SignalStudioUxR3AxisTests'}elseif($Suite -eq 'Ui'){'SignalStudioUiTests'}elseif($Suite -eq 'Ux'){'SignalStudioUxTests'}else{'SignalStudioNarrowbandAnalysisUiTests'}
$exe=Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/$target.exe"
$names=@('QT_QPA_PLATFORM','QT_SCALE_FACTOR','QT_QPA_FONTDIR','SS_NARROWBAND_NODE','SS_NARROWBAND_FIXTURES','SS_MODULATION_FIXTURES','SS_UI_TIMING_LOG')
$saved=@{}
try{
    foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
    $env:QT_QPA_PLATFORM='offscreen';$env:QT_SCALE_FACTOR='1.5';$env:QT_QPA_FONTDIR=Join-Path $env:SystemRoot 'Fonts'
    $env:SS_NARROWBAND_NODE='W8'
    $env:SS_NARROWBAND_FIXTURES=Join-Path $repoRoot 'out/tasks/wideband-detection/NB-W0/assets/fixtures'
    $env:SS_MODULATION_FIXTURES=Join-Path $repoRoot 'out/tasks/wideband-detection/NB-FEEDBACK-01/assets/modulation-fixtures'
    $log=Join-Path $OutputDirectory "$Configuration-$Suite-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff').txt"
    $env:SS_UI_TIMING_LOG="$log.timing.jsonl"
    & $exe @Cases -o "$log,txt" -o "$log.xml,junitxml"
    $LASTEXITCODE | Set-Content -LiteralPath "$log.exit-code" -Encoding utf8
    if($LASTEXITCODE -ne 0){throw "Directed Qt suite failed; actual log: $log"}
    Write-Output "Directed suite passed: $log"
}finally{foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}
