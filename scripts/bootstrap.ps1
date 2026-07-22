[CmdletBinding()]
param([switch]$Headless)

. (Join-Path $PSScriptRoot 'common.ps1')

Import-SignalStudioMsvcEnvironment
$cmake = Get-SignalStudioCMake
$ninja = Get-SignalStudioNinja
$git = Resolve-SignalStudioTool -Name 'git.exe' -Candidates @('D:\softwares\Git\cmd\git.exe')
$python = Resolve-SignalStudioTool -Name 'python.exe'
$qtRoot = $null
$qmake = $null
$qtVersion = 'not required for headless bootstrap'
if (-not $Headless) {
    $qtRoot = Initialize-SignalStudioQtEnvironment
    $qmake = Join-Path $qtRoot 'bin\qmake.exe'
    $qtVersion = (& $qmake -query QT_VERSION)
}
$userPresets = Update-SignalStudioUserPresets -NinjaPath $ninja -QtRoot $qtRoot

$cl = Get-Command cl.exe -ErrorAction Stop
$cuda = Get-Command nvcc.exe -ErrorAction SilentlyContinue
$gpu = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue

$report = [ordered]@{
    repository = $script:RepositoryRoot
    cmake = (& $cmake --version | Select-Object -First 1)
    ninja = (& $ninja --version)
    git = (& $git --version)
    python = (& $python --version 2>&1)
    msvc = $cl.Source
    qt = $qtVersion
    qt_root = $qtRoot
    qmake = $qmake
    nvidia_driver_present = [bool]$gpu
    cuda_toolkit_available = [bool]$cuda
    cuda_policy = 'optional; never installed automatically because acceptance of the NVIDIA EULA is required'
    selected_dependency_source = 'vcpkg.json pinned to 82b6bc886d7b0f8342e34babc2e0b8943f79b0e1'
    active_ms00_dependency_mode = if ($Headless) { 'headless standard C++20; Qt not discovered' } else { 'installed Qt plus standard C++20; deterministic in-repo test harness' }
    generated_user_presets = $userPresets
}

$report | ConvertTo-Json -Depth 4
if ($Headless) {
    $dependencyReport = & (Join-Path $PSScriptRoot 'validate-dependency-lock.ps1') -Mode CompatibleHost -Headless
} else {
    $dependencyReport = & (Join-Path $PSScriptRoot 'validate-dependency-lock.ps1') -Mode CompatibleHost
}
if ($LASTEXITCODE -ne 0) { throw 'Dependency lock validation failed.' }
$dependencyReport
Write-Host 'Bootstrap validation passed. No system state was changed.'
