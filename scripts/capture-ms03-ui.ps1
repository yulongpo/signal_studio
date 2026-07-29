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
foreach ($name in @(
    'QT_QPA_PLATFORM',
    'QT_QPA_PLATFORM_PLUGIN_PATH',
    'QT_QPA_FONTDIR',
    'QT_PLUGIN_PATH',
    'QT_SCALE_FACTOR',
    'QT_SCALE_FACTOR_ROUNDING_POLICY'
)) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}

function Invoke-SignalStudioCapture {
    param(
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ScaleFactor = '',
        [ValidateSet('p02', 'p04', 'p07')][string]$Page = 'p02',
        [switch]$UseOffscreen
    )

    [Environment]::SetEnvironmentVariable(
        'QT_QPA_PLATFORM',
        $(if ($UseOffscreen) { 'offscreen' } else { $null }),
        'Process')
    [Environment]::SetEnvironmentVariable(
        'QT_QPA_PLATFORM_PLUGIN_PATH',
        $(if ($UseOffscreen) { (Join-Path (Split-Path $executable) 'platforms') } else { $null }),
        'Process')
    [Environment]::SetEnvironmentVariable(
        'QT_QPA_FONTDIR',
        $(if ($UseOffscreen) { (Join-Path $env:SystemRoot 'Fonts') } else { $null }),
        'Process')
    [Environment]::SetEnvironmentVariable(
        'QT_SCALE_FACTOR',
        $(if ($ScaleFactor) { $ScaleFactor } else { $null }),
        'Process')
    [Environment]::SetEnvironmentVariable('QT_SCALE_FACTOR_ROUNDING_POLICY', 'PassThrough', 'Process')
    $output = Join-Path $resolvedOutput $Name
    $process = Start-Process -FilePath $executable -ArgumentList @(
        '--screenshot', $output,
        '--width', $Width,
        '--height', $Height,
        '--page', $Page
    ) -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "截图失败，退出码 $($process.ExitCode)：$output"
    }
    $file = Get-Item -LiteralPath $output
    if ($file.Length -le 0) {
        throw "截图为空：$output"
    }
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($output)
    try {
        $physicalWidth = $image.Width
        $physicalHeight = $image.Height
    }
    finally {
        $image.Dispose()
    }
    $script:captureManifest += [ordered]@{
        页面 = $Page.ToUpperInvariant()
        文件 = $Name
        逻辑宽度 = $Width
        逻辑高度 = $Height
        请求缩放 = if ($ScaleFactor) {
            [double]::Parse($ScaleFactor, [Globalization.CultureInfo]::InvariantCulture)
        } else {
            'Windows 当前显示器'
        }
        平台 = if ($UseOffscreen) { 'Qt offscreen 确定性回归' } else { 'Windows 当前平台' }
        物理宽度 = $physicalWidth
        物理高度 = $physicalHeight
        实际横向像素比 = [Math]::Round($physicalWidth / $Width, 4)
        实际纵向像素比 = [Math]::Round($physicalHeight / $Height, 4)
        文件字节 = $file.Length
    }
    Write-Output "$Name`t逻辑 ${Width}×${Height}`t物理 ${physicalWidth}×${physicalHeight}`t$($file.Length) 字节"
}

try {
    $captureManifest = @()
    Invoke-SignalStudioCapture -Width 1280 -Height 720 -Name 'MS-03_P02_1280x720_100百分比.png' -ScaleFactor '1' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1600 -Height 900 -Name 'MS-03_P02_1600x900_100百分比.png' -ScaleFactor '1' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1920 -Height 1080 -Name 'MS-03_P02_1920x1080_100百分比.png' -ScaleFactor '1' -UseOffscreen
    Invoke-SignalStudioCapture -Width 3840 -Height 2160 -Name 'MS-03_P02_3840x2160_100百分比.png' -ScaleFactor '1' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1920 -Height 1080 -Name 'MS-03_P02_1920x1080_125百分比.png' -ScaleFactor '1.25' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1920 -Height 1080 -Name 'MS-03_P02_1920x1080_150百分比.png' -ScaleFactor '1.5' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1920 -Height 1080 -Name 'MS-03_P02_1920x1080_175百分比.png' -ScaleFactor '1.75' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1920 -Height 1080 -Name 'MS-03_P02_1920x1080_200百分比.png' -ScaleFactor '2' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1600 -Height 900 -Name 'MS-03_P04_1600x900_100百分比.png' -ScaleFactor '1' -Page 'p04' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1600 -Height 900 -Name 'MS-03_P07_1600x900_100百分比.png' -ScaleFactor '1' -Page 'p07' -UseOffscreen
    Invoke-SignalStudioCapture -Width 1600 -Height 900 -Name 'MS-03_P02_1600x900_Windows当前DPI.png'
    $manifestPath = Join-Path $resolvedOutput 'MS-03_截图清单.json'
    $captureManifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    Write-Output "截图清单`t$manifestPath"
}
finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
