[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()]
    [string]$BuildDirectory = 'build/local-windows-msvc-cpu-release',

    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory = 'docs/development/ui-preview'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$resolvedBuild = (Resolve-Path -LiteralPath (Join-Path $repositoryRoot $BuildDirectory)).Path
$resolvedOutput = Join-Path $repositoryRoot $OutputDirectory
$executable = Join-Path $resolvedBuild 'bin/signal_visualization_workbench_demo.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "未找到 MS-03 工作台演示程序：$executable"
}

New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null

$savedEnvironment = @{}
foreach ($name in @('QT_QPA_PLATFORM', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_PLUGIN_PATH', 'QT_SCALE_FACTOR')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}

function Invoke-SignalStudioCapture {
    param(
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ScaleFactor = ''
    )

    [Environment]::SetEnvironmentVariable(
        'QT_SCALE_FACTOR',
        $(if ($ScaleFactor) { $ScaleFactor } else { $null }),
        'Process')
    $output = Join-Path $resolvedOutput $Name
    $process = Start-Process -FilePath $executable -ArgumentList @(
        '--screenshot', $output,
        '--width', $Width,
        '--height', $Height
    ) -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "截图失败，退出码 $($process.ExitCode)：$output"
    }
    $file = Get-Item -LiteralPath $output
    if ($file.Length -le 0) {
        throw "截图为空：$output"
    }
    Write-Output "$Name`t$($file.Length) 字节"
}

try {
    Invoke-SignalStudioCapture -Width 1280 -Height 720 -Name 'MS-03_工作台_1280x720.png'
    Invoke-SignalStudioCapture -Width 1600 -Height 900 -Name 'MS-03_工作台_1600x900.png'
    Invoke-SignalStudioCapture -Width 1920 -Height 1080 -Name 'MS-03_工作台_1920x1080.png'
    Invoke-SignalStudioCapture -Width 1280 -Height 720 -Name 'MS-03_工作台_200百分比DPI.png' -ScaleFactor '2'
}
finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
