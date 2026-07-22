[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [switch]$RequireQt
)

. (Join-Path $RepositoryRoot 'scripts\common.ps1')

Import-SignalStudioMsvcEnvironment
$ninja = Initialize-SignalStudioNinjaEnvironment
$qtRoot = if ($RequireQt) { Initialize-SignalStudioQtEnvironment } else { '' }
$firstPath = $env:Path
$firstInclude = $env:INCLUDE

Import-SignalStudioMsvcEnvironment
$null = Initialize-SignalStudioNinjaEnvironment
if ($RequireQt) { $null = Initialize-SignalStudioQtEnvironment }
$secondPath = $env:Path

if ($firstPath -cne $secondPath) { throw 'MSVC/Qt/Ninja environment import is not idempotent in one process' }
if ($firstInclude -cne $env:INCLUDE) { throw 'Repeated MSVC import changed INCLUDE' }
if ($env:SIGNAL_STUDIO_MSVC_ENVIRONMENT -ne 'vs2022-msvc-x64-v2') { throw 'MSVC import marker missing' }
if (-not $env:VSCMD_VER -or -not $env:VSINSTALLDIR) { throw 'MSVC initialization metadata missing' }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe missing after MSVC import' }
if ($env:VSCMD_ARG_TGT_ARCH -cne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -cne 'x64') {
    throw 'MSVC import did not select an x64-hosted x64 environment'
}

$entries = @($secondPath -split ';' | Where-Object { $_ })
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $entries) {
    if (-not $seen.Add($entry.TrimEnd('\', '/'))) { throw "Duplicate PATH entry detected: $entry" }
}
if ($secondPath.Length -gt 16384) { throw "PATH length regression: $($secondPath.Length)" }

$presetPath = Update-SignalStudioUserPresets -NinjaPath $ninja -QtRoot $qtRoot
$firstPresetBytes = [IO.File]::ReadAllBytes($presetPath)
$firstPresetBase64 = [Convert]::ToBase64String($firstPresetBytes)
for ($iteration = 0; $iteration -lt 3; $iteration++) {
    $null = Update-SignalStudioUserPresets -NinjaPath $ninja -QtRoot $qtRoot
    $nextPresetBytes = [IO.File]::ReadAllBytes($presetPath)
    if ([Convert]::ToBase64String($nextPresetBytes) -cne $firstPresetBase64) {
        throw "CMakeUserPresets.json changed during repeated generation at iteration $iteration"
    }
}
if ($firstPresetBytes.Length -gt 32768) {
    throw "CMakeUserPresets.json unexpectedly embeds repeated environment content: $($firstPresetBytes.Length) bytes"
}

$presetDocument = Get-Content -Raw -Encoding UTF8 -LiteralPath $presetPath | ConvertFrom-Json
$configureNames = @($presetDocument.configurePresets | ForEach-Object { $_.name })
if (@($configureNames | Select-Object -Unique).Count -ne $configureNames.Count) {
    throw 'CMakeUserPresets.json contains duplicate configure preset names'
}
foreach ($preset in $presetDocument.configurePresets) {
    if (-not ($preset.PSObject.Properties.Name -contains 'environment')) { continue }
    foreach ($variable in @('PATH', 'CMAKE_PREFIX_PATH', 'INCLUDE', 'LIB', 'LIBPATH')) {
        if (-not ($preset.environment.PSObject.Properties.Name -contains $variable)) { continue }
        $value = $preset.environment.$variable
        $pathEntries = @($value -split ';' | Where-Object { $_ })
        $pathSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($pathEntry in $pathEntries) {
            $canonical = Get-SignalStudioCanonicalPathEntry -PathEntry $pathEntry
            if (-not $pathSet.Add($canonical)) {
                throw "Duplicate $variable entry in preset $($preset.name): $pathEntry"
            }
        }
    }
}
if ($RequireQt) {
    $basePreset = $presetDocument.configurePresets | Where-Object { $_.name -eq 'local-msvc-toolchain-base' }
    $qtPreset = $presetDocument.configurePresets | Where-Object { $_.name -eq 'local-msvc-qt-toolchain-base' }
    $canonicalQt = Get-SignalStudioCanonicalPathEntry -PathEntry $qtRoot
    $prefixQtEntries = @($basePreset.environment.CMAKE_PREFIX_PATH -split ';' | Where-Object {
        (Get-SignalStudioCanonicalPathEntry -PathEntry $_).Equals($canonicalQt, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($prefixQtEntries.Count -ne 1 -or
        -not (Get-SignalStudioCanonicalPathEntry -PathEntry $qtPreset.environment.SIGNAL_STUDIO_QT_ROOT).Equals(
            $canonicalQt, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Qt root is not represented exactly once in the generated Qt toolchain preset'
    }
}

Write-Host "Verified idempotent toolchain import and stable user presets: PATH entries=$($entries.Count) length=$($secondPath.Length), preset bytes=$($firstPresetBytes.Length)"
