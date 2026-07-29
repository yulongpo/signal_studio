[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$Preset = 'windows-msvc-debug',
    [ValidateRange(1, 256)][int]$Parallel = [Environment]::ProcessorCount,
    [string]$Target = '',
    [switch]$CleanFirst
)

. (Join-Path $PSScriptRoot 'common.ps1')
Assert-SignalStudioPreset -Preset $Preset
Import-SignalStudioMsvcEnvironment
$cmake = Get-SignalStudioCMake
$null = Initialize-SignalStudioNinjaEnvironment
if (Test-SignalStudioUiPreset -Preset $Preset) {
    $null = Initialize-SignalStudioQtEnvironment
}

$cache = Join-Path $script:RepositoryRoot "build\$Preset\CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'configure.ps1') -Preset $Preset
}

$arguments = @('--build', '--preset', $Preset, '--parallel', $Parallel)
if (-not [string]::IsNullOrWhiteSpace($Target)) {
    $arguments += @('--target', $Target)
}
if ($CleanFirst) {
    $arguments += '--clean-first'
}
Push-Location $script:RepositoryRoot
try {
    & $cmake @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
