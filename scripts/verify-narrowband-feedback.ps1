[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug',[switch]$InputOnly,[switch]$NativeInput,[switch]$ModulationOnly,[switch]$GenerateModulationFixtures,[string]$UserSource,[switch]$NativeScene,[switch]$SoftwareRenderer,[switch]$RebuildGui)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repoRoot 'out/tasks/wideband-detection/NB-FEEDBACK-01'
foreach($part in @('logs','diagnostics','assets')){New-Item -ItemType Directory -Path (Join-Path $output $part) -Force|Out-Null}
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$env:SS_MODULATION_FIXTURES=Join-Path $output 'assets/modulation-fixtures'
$fixtureIndex=Join-Path $env:SS_MODULATION_FIXTURES 'modulation-index.json'
$fixturesCurrent=(Test-Path $fixtureIndex) -and ((Get-Content $fixtureIndex -Raw|ConvertFrom-Json).schemaVersion -eq 2)
if($GenerateModulationFixtures -or ((-not $InputOnly -and -not $NativeInput -and -not $NativeScene) -and -not $fixturesCurrent)) {
    & 'D:/profiles/anaconda/python.exe' (Join-Path $PSScriptRoot 'generate-modulation-fixtures.py') --output $env:SS_MODULATION_FIXTURES
    if($LASTEXITCODE -ne 0){throw 'Family/order fixture generation failed'}
}
if($NativeInput){
    $old=$env:QT_QPA_PLATFORM
    try {
        $env:QT_QPA_PLATFORM='windows'
        & (Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioParameterInputTests.exe") -o (Join-Path $output "logs/input-native-$stamp-$Configuration.txt,txt")
        if($LASTEXITCODE -ne 0){throw 'Native spin button regression failed'}
    } finally {$env:QT_QPA_PLATFORM=$old}
    return
}
if($NativeScene) {
    if(-not $UserSource){throw '-NativeScene requires the user WAV path'}
    $nativeRoot=Join-Path $output "diagnostics/native-$stamp-$Configuration-$(if($SoftwareRenderer){'software'}else{'gpu'})"
    New-Item -ItemType Directory -Path $nativeRoot -Force|Out-Null
    $names=@('PATH','QT_QPA_PLATFORM','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS','QT_SCALE_FACTOR_ROUNDING_POLICY','QT_QPA_FONTDIR','SS_FEEDBACK_SOURCE')
    $saved=@{};foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
    try {
        foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
        $env:PATH="$env:SystemRoot/System32;$env:SystemRoot;$env:SystemRoot/System32/Wbem";$env:QT_QPA_PLATFORM='windows';$env:SS_FEEDBACK_SOURCE=$UserSource
        $arguments=@('--narrowband-feedback',$nativeRoot);if($SoftwareRenderer){$arguments+='--software-renderer'}
        & (Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioUiCapture.exe") @arguments 2>&1|Out-File (Join-Path $output "logs/native-$stamp-$Configuration.txt") -Encoding utf8
        if($LASTEXITCODE -ne 0){throw "Feedback native acceptance failed: $nativeRoot"}
        $report=Get-Content (Join-Path $nativeRoot 'feedback-report.json') -Raw|ConvertFrom-Json
        if(-not $report.pass -or -not $report.nativeAcceptance -or -not $report.layoutUnified -or $report.sceneCount -ne 7){throw 'Incomplete actual monitor feedback report'}
        Write-Host "Feedback actual monitor acceptance passed: $nativeRoot"
    } finally {foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}
    return
}
$targets=@('SignalStudioParameterInputTests');$regex='^signal_studio_parameter_input$'
if(-not $InputOnly){$targets+=@('SignalStudioNarrowbandAnalysisUiTests','SignalStudioNarrowbandRecoveryTests','SignalStudioNarrowbandModulationTests','SignalStudioNarrowbandAnalysisTests','SignalStudioChannelRateTests');$regex='^signal_studio_(parameter_input|narrowband_analysis_ui|narrowband_recovery|narrowband_modulation|narrowband_analysis|channel_rate)$'}
if($ModulationOnly){$targets=@('SignalStudioNarrowbandModulationTests');$regex='^signal_studio_narrowband_modulation$'}
if($UserSource){$targets+= 'SignalStudioNarrowbandFeedbackProbe'}
if($RebuildGui){$targets+=@('SignalStudio','SignalStudioUiCapture')}
$env:SS_NARROWBAND_NODE='W8'
$env:SS_NARROWBAND_REPORT_DIR=Join-Path $output "diagnostics/$stamp-$Configuration"
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Action All -Targets $targets -TestRegex $regex -VerboseTests *> (Join-Path $output "logs/directed-$stamp-$Configuration.txt")
if(-not $?){throw 'Narrowband feedback regression failed'}
if($UserSource) {
    & (Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioNarrowbandFeedbackProbe.exe") $UserSource (Join-Path $output "diagnostics/user-source-$stamp-$Configuration.json") *> (Join-Path $output "logs/user-source-$stamp-$Configuration.txt")
    if($LASTEXITCODE -ne 0){throw 'User source reproduction failed'}
}
