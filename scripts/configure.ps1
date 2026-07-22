[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$Preset = 'windows-msvc-debug',
    [switch]$Fresh
)

. (Join-Path $PSScriptRoot 'common.ps1')
Assert-SignalStudioPreset -Preset $Preset
Import-SignalStudioMsvcEnvironment
$cmake = Get-SignalStudioCMake
$ninja = Initialize-SignalStudioNinjaEnvironment
$qtRoot = $null
if (Test-SignalStudioUiPreset -Preset $Preset) {
    $qtRoot = Initialize-SignalStudioQtEnvironment
}
$null = Update-SignalStudioUserPresets -NinjaPath $ninja -QtRoot $qtRoot

$arguments = @('--preset', $Preset)
if ($qtRoot) {
    $arguments += "-DSIGNAL_STUDIO_QT_ROOT=$qtRoot"
}
if ($Fresh) {
    $arguments += '--fresh'
}
Push-Location $script:RepositoryRoot
try {
    & $cmake @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
