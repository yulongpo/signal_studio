[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$Preset = 'windows-msvc-debug',
    [string]$Regex = '',
    [switch]$VerboseOutput
)

. (Join-Path $PSScriptRoot 'common.ps1')
Assert-SignalStudioPreset -Preset $Preset
Import-SignalStudioMsvcEnvironment
$ctest = Get-SignalStudioCTest
if (Test-SignalStudioUiPreset -Preset $Preset) {
    $null = Initialize-SignalStudioQtEnvironment
}
$cache = Join-Path $script:RepositoryRoot "build\$Preset\CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
    throw "Preset '$Preset' has not been configured. Run scripts/configure.ps1 first."
}

$arguments = @('--preset', $Preset, '--output-on-failure')
if ($Regex) {
    $arguments += @('-R', $Regex)
}
if ($VerboseOutput) {
    $arguments += '--verbose'
}
Push-Location $script:RepositoryRoot
try {
    & $ctest @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
