[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('Configure', 'Build', 'Test', 'All')]
    [string]$Action = 'All',
    [string]$Preset,
    [string]$CMake = 'cmake',
    [int]$Jobs = 0,
    [string[]]$Targets = @(),
    [string]$TestRegex,
    [switch]$ListTests,
    [string[]]$UiCases = @(),
    [string]$Python = 'D:/profiles/anaconda/python.exe'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Preset) {
    $configName = $Configuration.ToLowerInvariant()
    $userPresetPath = Join-Path $repoRoot 'CMakeUserPresets.json'
    $localPreset = "local-$configName"
    $hasLocalPreset = $false
    if (Test-Path -LiteralPath $userPresetPath) {
        $userPresets = Get-Content -LiteralPath $userPresetPath -Raw | ConvertFrom-Json
        $hasLocalPreset = @($userPresets.configurePresets.name) -contains $localPreset
    }
    $Preset = if ($hasLocalPreset) { $localPreset } else { "vs2026-qt611-$configName" }
}
$cmakeExecutable = (Get-Command $CMake -ErrorAction Stop).Source
$ctestExecutable = Join-Path (Split-Path -Parent $cmakeExecutable) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctestExecutable)) {
    $ctestExecutable = (Get-Command 'ctest' -ErrorAction Stop).Source
}
function Invoke-CheckedCommand([string]$Executable, [string[]]$CommandArguments) {
    Write-Host "$Executable $($CommandArguments -join ' ')"
    & $Executable @CommandArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Executable"
    }
}
Push-Location -LiteralPath $repoRoot
try {
    if ($Action -in @('Configure', 'All')) {
        Invoke-CheckedCommand $cmakeExecutable @('--preset', $Preset)
    }
    if ($Action -in @('Build', 'All')) {
        $buildArguments = @('--build', '--preset', $Preset)
        if ($Targets.Count -gt 0) { $buildArguments += @('--target') + $Targets }
        if ($Jobs -gt 0) { $buildArguments += @('--parallel', "$Jobs") }
        Invoke-CheckedCommand $cmakeExecutable $buildArguments
    }
    if ($Action -in @('Test', 'All')) {
        if (-not $ListTests -and (-not $TestRegex -or $TestRegex -match 'narrowband|channel_rate')) {
            if (-not $env:SS_NARROWBAND_FIXTURES) { $env:SS_NARROWBAND_FIXTURES = Join-Path $repoRoot 'out/tasks/wideband-detection/NB-W0/assets/fixtures' }
            foreach ($fixture in @(@('index.json',''),@('edge-index.json','--extra-only'),@('recovery-index.json','--recovery-only'))) {
                if (-not (Test-Path -LiteralPath (Join-Path $env:SS_NARROWBAND_FIXTURES $fixture[0]))) {
                    $generatorArguments = @((Join-Path $PSScriptRoot 'generate-narrowband-fixtures.py'),'--output',$env:SS_NARROWBAND_FIXTURES)
                    if ($fixture[1]) { $generatorArguments += $fixture[1] }
                    Invoke-CheckedCommand $Python $generatorArguments
                }
            }
        }
        $testArguments = @('--preset', $Preset, '--output-on-failure')
        if ($TestRegex) { $testArguments += @('-R', $TestRegex) }
        if ($ListTests) { $testArguments += '-N' }
        $previousUiCases = $env:SS_UI_CASES
        try {
            if ($UiCases.Count -gt 0) { $env:SS_UI_CASES = $UiCases -join ';' }
            Invoke-CheckedCommand $ctestExecutable $testArguments
        } finally { $env:SS_UI_CASES = $previousUiCases }
    }
}
finally {
    Pop-Location
}
