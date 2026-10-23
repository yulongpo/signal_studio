[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [ValidateSet('W0','W1','W2','W3','W4','W5','W6','W7','W8')][string]$Node = 'W0',
    [switch]$GenerateFixtures,
    [switch]$GenerateEdgeFixtures,
    [switch]$GenerateRecoveryFixtures,
    [switch]$GenerateRateFixtures,
    [switch]$FixturesOnly,
    [switch]$Native,
    [switch]$SoftwareRenderer,
    [switch]$AuxiliaryDisplay,
    [switch]$ProbeScreens,
    [switch]$UiRetry,
    [switch]$RebuildGui,
    [string]$OutputDirectory,
    [int]$Jobs = 0,
    [string]$Python = 'D:/profiles/anaconda/python.exe'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot "out/tasks/wideband-detection/NB-$Node" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($part in @('logs','diagnostics','assets')) { New-Item -ItemType Directory -Path (Join-Path $OutputDirectory $part) -Force | Out-Null }
$fixtureRoot = Join-Path $repoRoot 'out/tasks/wideband-detection/NB-W0/assets/fixtures'
$runName = "$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')-$Configuration"
function Invoke-NarrowbandChecked([string]$Program, [string[]]$CommandArguments) {
    & $Program @CommandArguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location -LiteralPath $repoRoot
$previousFinalUiCases = $env:SS_UI_CASES
try {
    if ($Node -eq 'W8') { $env:SS_UI_CASES = $null }
    if ($GenerateFixtures) {
        Invoke-NarrowbandChecked $Python @((Join-Path $PSScriptRoot 'generate-narrowband-fixtures.py'), '--output', $fixtureRoot)
    }
    if ($GenerateEdgeFixtures) {
        Invoke-NarrowbandChecked $Python @((Join-Path $PSScriptRoot 'generate-narrowband-fixtures.py'), '--output', $fixtureRoot, '--extra-only')
    }
    if ($GenerateRecoveryFixtures) {
        Invoke-NarrowbandChecked $Python @((Join-Path $PSScriptRoot 'generate-narrowband-fixtures.py'), '--output', $fixtureRoot, '--recovery-only')
    }
    if ($GenerateRateFixtures) {
        Invoke-NarrowbandChecked $Python @((Join-Path $PSScriptRoot 'generate-narrowband-fixtures.py'), '--output', $fixtureRoot, '--rates-only')
    }
    if ($FixturesOnly) { return }
    if ($ProbeScreens) {
        Invoke-NarrowbandChecked (Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioUiCapture.exe") @('--list-screens')
        return
    }
    if ($Native) {
        $arguments = @{Configuration=$Configuration; NarrowbandAnalysis=$true; OutputDirectory=(Join-Path $OutputDirectory "diagnostics/native-$runName"); SoftwareRenderer=$SoftwareRenderer.IsPresent; AuxiliaryDisplay=$AuxiliaryDisplay.IsPresent}
        & (Join-Path $PSScriptRoot 'screenshot.ps1') @arguments
        if (-not $?) { throw 'Native narrowband analysis acceptance failed' }
        return
    }
    $env:SS_NARROWBAND_FIXTURES = $fixtureRoot
    $env:SS_NARROWBAND_REPORT_DIR = Join-Path $OutputDirectory "diagnostics/$runName"
    New-Item -ItemType Directory -Path $env:SS_NARROWBAND_REPORT_DIR -Force | Out-Null
    $env:SS_NARROWBAND_NODE = $Node
    if ($UiRetry) {
        if ($Node -ne 'W8') { throw '-UiRetry is a final acceptance diagnostic only' }
        $buildRoot = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())"
        foreach ($suffix in @('.txt','.txt.xml','.txt.timing.jsonl')) {
            $previous = Join-Path $buildRoot "ui-test-results-$($Configuration.ToLowerInvariant())$suffix"
            if (Test-Path -LiteralPath $previous) { Copy-Item -LiteralPath $previous -Destination (Join-Path $OutputDirectory "logs/previous-ui-$runName$suffix") }
        }
        if ($RebuildGui) {
            & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Action All -Targets SignalStudioUiTests,SignalStudioNarrowbandAnalysisUiTests,SignalStudioUiCapture,SignalStudioUxTests -TestRegex 'signal_studio_(ui(_.*)?|ux_.*|narrowband_analysis_ui)$' *> (Join-Path $OutputDirectory "logs/ui-retest-$runName.txt")
        } else {
            & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Action Test -TestRegex '^signal_studio_ui(_.*)?$' *> (Join-Path $OutputDirectory "logs/ui-retest-$runName.txt")
        }
        if (-not $?) { throw 'Final standalone UI retry failed' }
        Write-Host "Final $Configuration UI retry passed: $OutputDirectory/logs/ui-retest-$runName.txt"
        return
    }
    if ($Node -eq 'W0') {
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Action All -Targets SignalStudioChannelProcessorTests -TestRegex '^signal_studio_channel_processor$' *> (Join-Path $OutputDirectory 'logs/baseline.txt')
    } elseif ($Node -eq 'W8') {
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Action All -Jobs $Jobs *> (Join-Path $OutputDirectory "logs/final-$runName.txt")
    } else {
        $testRegex = if ($Node -eq 'W1') { 'signal_studio_(channel_rate|channel_processor|core|annotations|annotation_store|narrowband_analysis_ui)$' } elseif ($Node -eq 'W2') { 'signal_studio_(narrowband_analysis|channel_processor|channel_rate)$' } elseif ($Node -in @('W6','W7')) { 'signal_studio_narrowband_(analysis|recovery)' } else { 'signal_studio_narrowband_analysis' }
        [string[]]$targets = if ($Node -eq 'W1') { @('SignalStudioChannelRateTests','SignalStudioChannelProcessorTests','SignalStudioCoreTests','SignalStudioAnnotationTests','SignalStudioAnnotationStoreTests','SignalStudioNarrowbandAnalysisUiTests') } else { @('SignalStudioNarrowbandAnalysisTests') }
        if ($Node -in @('W3','W4','W5','W6','W7')) { $targets += 'SignalStudioNarrowbandAnalysisUiTests' }
        if ($Node -eq 'W2') { $targets += @('SignalStudioChannelProcessorTests','SignalStudioChannelRateTests') }
        if ($Node -in @('W6','W7')) { $targets += 'SignalStudioNarrowbandRecoveryTests' }
        if ($Node -eq 'W7') { $targets += 'SignalStudioChannelRateTests'; $testRegex = 'signal_studio_(channel_rate|narrowband_(analysis|recovery).*)$' }
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Action All -Targets $targets -TestRegex $testRegex *> (Join-Path $OutputDirectory "logs/directed-$runName.txt")
    }
    if (-not $?) { throw "Narrowband $Node validation failed" }
    Write-Host "$Node $Configuration validation completed. Logs: $OutputDirectory/logs"
} finally { $env:SS_UI_CASES = $previousFinalUiCases; Pop-Location }
