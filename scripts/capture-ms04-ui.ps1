[CmdletBinding()]
param(
    [string]$Preset = "local-windows-msvc-cpu-release",
    [string]$OutputDirectory = "",
    [string]$InputFile = "",
    [switch]$SkipBuild,
    [switch]$UseOffscreen
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot "docs\milestones\MS-04\evidence\ui"
}
if ([string]::IsNullOrWhiteSpace($InputFile)) {
    $InputFile = Join-Path (Split-Path -Parent $repositoryRoot) "test_data\x310_capture_cf1425MHz_sr50MSps_20260521_144220.sc16"
}
$executable = Join-Path $repositoryRoot "build\$Preset\bin\SignalStudio.exe"
if (-not $SkipBuild) {
    & cmake --build --preset $Preset --target signal_studio
    if ($LASTEXITCODE -ne 0) {
        throw "SignalStudio build failed."
    }
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "SignalStudio executable was not found: $executable"
}
if (-not (Test-Path -LiteralPath $InputFile -PathType Leaf)) {
    throw "Approved test recording was not found: $InputFile"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stateRoot = Join-Path $repositoryRoot "build\ms04-screenshot-state"
New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
Add-Type -AssemblyName System.Drawing
$captureRecords = @()
$nativeDpr = 1.0

if (-not $UseOffscreen) {
    $probeOutput = Join-Path $stateRoot "windows-dpi-probe.stdout.log"
    $probeError = Join-Path $stateRoot "windows-dpi-probe.stderr.log"
    $probeState = Join-Path $stateRoot "windows-dpi-probe"
    $previousScale = $env:QT_SCALE_FACTOR
    $previousPlatform = $env:QT_QPA_PLATFORM
    try {
        Remove-Item Env:QT_SCALE_FACTOR -ErrorAction SilentlyContinue
        Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
        $probe = Start-Process -FilePath $executable `
            -ArgumentList @("--startup-smoke", "--state-dir", $probeState) `
            -RedirectStandardOutput $probeOutput `
            -RedirectStandardError $probeError `
            -Wait -PassThru
        if ($probe.ExitCode -ne 0) {
            throw "Windows DPI probe failed with exit code $($probe.ExitCode)."
        }
        $probeText = Get-Content -LiteralPath $probeOutput -Raw
        if ($probeText -notmatch "platform=windows\s+dpr=([0-9.]+)") {
            throw "Windows DPI probe did not report a valid DPR: $probeText"
        }
        $nativeDpr = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        if ($nativeDpr -le 0.0) {
            throw "Windows DPI probe reported an invalid DPR: $nativeDpr"
        }
    }
    finally {
        if ($null -eq $previousScale) {
            Remove-Item Env:QT_SCALE_FACTOR -ErrorAction SilentlyContinue
        }
        else {
            $env:QT_SCALE_FACTOR = $previousScale
        }
        if ($null -eq $previousPlatform) {
            Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
        }
        else {
            $env:QT_QPA_PLATFORM = $previousPlatform
        }
    }
}

function Invoke-SignalStudioCapture {
    param(
        [string]$Name,
        [string]$Page,
        [int]$Width,
        [int]$Height,
        [string]$Scale = "",
        [switch]$NeedsInput
    )
    $output = Join-Path $OutputDirectory "$Name.png"
    $state = Join-Path $stateRoot $Name
    $resolvedStateRoot = [IO.Path]::GetFullPath($stateRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $resolvedState = [IO.Path]::GetFullPath($state)
    if (-not $resolvedState.StartsWith($resolvedStateRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Screenshot state directory escaped the approved build root: $resolvedState"
    }
    if (Test-Path -LiteralPath $resolvedState -PathType Container) {
        Remove-Item -LiteralPath $resolvedState -Recurse -Force
    }
    $arguments = @(
        "--screenshot", $output,
        "--page", $Page,
        "--width", $Width,
        "--height", $Height,
        "--state-dir", $resolvedState
    )
    if ($NeedsInput) {
        $arguments += @("--input", $InputFile)
    }
    $previousScale = $env:QT_SCALE_FACTOR
    $previousPolicy = $env:QT_SCALE_FACTOR_ROUNDING_POLICY
    $previousPlatform = $env:QT_QPA_PLATFORM
    $previousPluginPath = $env:QT_PLUGIN_PATH
    $previousPlatformPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    try {
        if ($UseOffscreen) {
            $env:QT_QPA_PLATFORM = "offscreen"
            $env:QT_PLUGIN_PATH = Split-Path -Parent $executable
            $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path (Split-Path -Parent $executable) "platforms"
        }
        else {
            Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
            Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
            Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
        }
        $effectiveScale = if ([string]::IsNullOrWhiteSpace($Scale)) { 1.0 } else {
            [double]::Parse($Scale, [Globalization.CultureInfo]::InvariantCulture)
        }
        $qtScale = if ($UseOffscreen) { $effectiveScale } else { $effectiveScale / $nativeDpr }
        $env:QT_SCALE_FACTOR = $qtScale.ToString("0.############", [Globalization.CultureInfo]::InvariantCulture)
        $env:QT_SCALE_FACTOR_ROUNDING_POLICY = "PassThrough"
        $process = Start-Process -FilePath $executable -ArgumentList $arguments -Wait -PassThru
        if ($process.ExitCode -ne 0) {
            throw "Screenshot capture failed: $Name (exit code $($process.ExitCode))"
        }
        $image = [System.Drawing.Image]::FromFile($output)
        try {
            $expectedWidth = [int][Math]::Round($Width * $effectiveScale)
            $expectedHeight = [int][Math]::Round($Height * $effectiveScale)
            if ($image.Width -ne $expectedWidth -or $image.Height -ne $expectedHeight) {
                throw "Screenshot dimensions mismatch: $Name actual=$($image.Width)x$($image.Height) expected=${expectedWidth}x${expectedHeight}"
            }
            $script:captureRecords += [ordered]@{
                名称 = $Name
                页面 = $Page
                逻辑宽度 = $Width
                逻辑高度 = $Height
                缩放因子 = $effectiveScale
                物理宽度 = $image.Width
                物理高度 = $image.Height
                平台 = if ($UseOffscreen) { "offscreen" } else { "windows" }
                Windows原生DPR = if ($UseOffscreen) { $null } else { $nativeDpr }
                输入 = if ($NeedsInput) { [IO.Path]::GetFileName($InputFile) } else { "" }
            }
        }
        finally {
            $image.Dispose()
        }
    }
    finally {
        if ($null -eq $previousScale) {
            Remove-Item Env:QT_SCALE_FACTOR -ErrorAction SilentlyContinue
        }
        else {
            $env:QT_SCALE_FACTOR = $previousScale
        }
        if ($null -eq $previousPolicy) {
            Remove-Item Env:QT_SCALE_FACTOR_ROUNDING_POLICY -ErrorAction SilentlyContinue
        }
        else {
            $env:QT_SCALE_FACTOR_ROUNDING_POLICY = $previousPolicy
        }
        foreach ($entry in @(
            @("QT_QPA_PLATFORM", $previousPlatform),
            @("QT_PLUGIN_PATH", $previousPluginPath),
            @("QT_QPA_PLATFORM_PLUGIN_PATH", $previousPlatformPluginPath)
        )) {
            if ($null -eq $entry[1]) {
                Remove-Item "Env:$($entry[0])" -ErrorAction SilentlyContinue
            }
            else {
                Set-Item "Env:$($entry[0])" $entry[1]
            }
        }
    }
}

Invoke-SignalStudioCapture -Name "MS-04_P01_1280x720" -Page "p01" -Width 1280 -Height 720
Invoke-SignalStudioCapture -Name "MS-04_P01_1920x1080" -Page "p01" -Width 1920 -Height 1080
Invoke-SignalStudioCapture -Name "MS-04_P01_3840x2160" -Page "p01" -Width 3840 -Height 2160
Invoke-SignalStudioCapture -Name "MS-04_W01_1600x900" -Page "w01" -Width 1600 -Height 900 -NeedsInput
Invoke-SignalStudioCapture -Name "MS-04_P02_1600x900" -Page "p02" -Width 1600 -Height 900 -NeedsInput
Invoke-SignalStudioCapture -Name "MS-04_P03_1600x900" -Page "p03" -Width 1600 -Height 900 -NeedsInput
Invoke-SignalStudioCapture -Name "MS-04_P05_1600x900" -Page "p05" -Width 1600 -Height 900 -NeedsInput
Invoke-SignalStudioCapture -Name "MS-04_W05_1600x900" -Page "w05" -Width 1600 -Height 900 -NeedsInput
Invoke-SignalStudioCapture -Name "MS-04_P02_150percent" -Page "p02" -Width 1920 -Height 1080 -Scale "1.5" -NeedsInput
Invoke-SignalStudioCapture -Name "MS-04_P02_200percent" -Page "p02" -Width 1920 -Height 1080 -Scale "2.0" -NeedsInput

$manifest = [ordered]@{
    schema = "signal-studio.ms04-ui-captures/1.0"
    说明 = "逻辑尺寸乘目标 DPR 必须等于 PNG 物理尺寸；标准证据使用 Windows 平台并按本机原生 DPR 归一化。"
    生成时间 = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ssK")
    截图 = $captureRecords
}
$manifestPath = Join-Path $OutputDirectory "MS-04_UI截图清单.json"
$manifestJson = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText($manifestPath, $manifestJson + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

Get-ChildItem -LiteralPath $OutputDirectory -Filter "MS-04_*.png" |
    Sort-Object Name |
    Select-Object Name, Length, LastWriteTime
