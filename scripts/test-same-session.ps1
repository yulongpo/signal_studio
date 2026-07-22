[CmdletBinding()]
param(
    [switch]$Headless,
    [switch]$SummaryOnly,
    [ValidateRange(1, 256)][int]$Parallel = [Environment]::ProcessorCount
)

. (Join-Path $PSScriptRoot 'common.ps1')

$debugPreset = if ($Headless) { 'windows-msvc-headless-debug' } else { 'windows-msvc-debug' }
$releasePreset = if ($Headless) { 'windows-msvc-headless-release' } else { 'windows-msvc-release' }
$results = [Collections.Generic.List[object]]::new()

foreach ($preset in @($debugPreset, $releasePreset)) {
    $started = Get-Date
    if ($SummaryOnly) {
        & (Join-Path $PSScriptRoot 'configure.ps1') -Preset $preset -Fresh | Out-Null
        & (Join-Path $PSScriptRoot 'build.ps1') -Preset $preset -Parallel $Parallel -CleanFirst | Out-Null
        $testOutput = (& (Join-Path $PSScriptRoot 'test.ps1') -Preset $preset | Out-String)
        $testCountMatch = [regex]::Match($testOutput, '100% tests passed, 0 tests failed out of ([0-9]+)')
        $testTimeMatch = [regex]::Match($testOutput, 'Total Test time \(real\) =\s+([0-9.]+) sec')
        if (-not $testCountMatch.Success -or -not $testTimeMatch.Success) {
            throw "CTest summary was not found for $preset"
        }
    } else {
        & (Join-Path $PSScriptRoot 'configure.ps1') -Preset $preset -Fresh
        & (Join-Path $PSScriptRoot 'build.ps1') -Preset $preset -Parallel $Parallel -CleanFirst
        & (Join-Path $PSScriptRoot 'test.ps1') -Preset $preset
    }
    $results.Add([ordered]@{
        preset = $preset
        elapsed_seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        tests_passed = if ($SummaryOnly) { [int]$testCountMatch.Groups[1].Value } else { 'see CTest output' }
        ctest_seconds = if ($SummaryOnly) { [double]$testTimeMatch.Groups[1].Value } else { 'see CTest output' }
    })
}

$pathEntries = @($env:Path -split ';' | Where-Object { $_ })
$uniqueEntries = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $pathEntries) {
    if (-not $uniqueEntries.Add($entry.TrimEnd('\', '/'))) {
        throw "Duplicate PATH entry survived same-session validation: $entry"
    }
}
if ($env:Path.Length -gt 16384) {
    throw "Same-session PATH length is unexpectedly large: $($env:Path.Length)"
}

[ordered]@{
    schema = 'signal-studio.same-session-test/1.0'
    process_id = $PID
    presets = $results
    path_entries = $pathEntries.Count
    unique_path_entries = $uniqueEntries.Count
    path_length = $env:Path.Length
    msvc_environment_marker = $env:SIGNAL_STUDIO_MSVC_ENVIRONMENT
    result = 'pass'
} | ConvertTo-Json -Depth 5
