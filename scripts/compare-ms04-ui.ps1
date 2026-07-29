[CmdletBinding()]
param(
    [string]$CurrentDirectory = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($CurrentDirectory)) {
    $CurrentDirectory = Join-Path $repositoryRoot "docs\milestones\MS-04\evidence\ui"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $CurrentDirectory "comparison"
}
$baselineRoot = Join-Path $repositoryRoot "docs\baseline\Signal-Studio-Dev-Docs"
$prototypeRoot = Get-ChildItem -LiteralPath $baselineRoot -Directory |
    Where-Object Name -Like "02_*" |
    Select-Object -First 1
$auditDirectory = Get-ChildItem -LiteralPath $prototypeRoot.FullName -Directory -Recurse |
    Where-Object Name -EQ "audit" |
    Select-Object -First 1
if (-not $auditDirectory) {
    throw "The approved audit screenshot directory was not found."
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Get-AuditScreenshot {
    param([string]$Prefix)
    $match = Get-ChildItem -LiteralPath $auditDirectory.FullName -Filter "$Prefix*.png" |
        Select-Object -First 1
    if (-not $match) {
        throw "Approved screenshot was not found: $Prefix"
    }
    return $match.FullName
}

function Add-FittedImage {
    param(
        [System.Drawing.Graphics]$Graphics,
        [System.Drawing.Image]$Image,
        [int]$PanelX,
        [int]$PanelWidth,
        [int]$CanvasHeight,
        [string]$Label
    )
    $headerHeight = 42
    $availableHeight = $CanvasHeight - $headerHeight
    $scale = [Math]::Min($PanelWidth / $Image.Width, $availableHeight / $Image.Height)
    $width = [Math]::Max(1, [int][Math]::Round($Image.Width * $scale))
    $height = [Math]::Max(1, [int][Math]::Round($Image.Height * $scale))
    $x = $PanelX + [int](($PanelWidth - $width) / 2)
    $y = $headerHeight + [int](($availableHeight - $height) / 2)
    $Graphics.DrawImage($Image, $x, $y, $width, $height)
    $font = New-Object System.Drawing.Font("Segoe UI", 16, [System.Drawing.FontStyle]::Bold)
    try {
        $Graphics.DrawString($Label, $font, [System.Drawing.Brushes]::White, $PanelX + 12, 8)
    }
    finally {
        $font.Dispose()
    }
}

function New-Comparison {
    param(
        [string]$Name,
        [string]$Baseline,
        [string]$Current
    )
    if (-not (Test-Path -LiteralPath $Current -PathType Leaf)) {
        throw "Current screenshot was not found: $Current"
    }
    $left = [System.Drawing.Image]::FromFile($Baseline)
    $right = [System.Drawing.Image]::FromFile($Current)
    $canvas = New-Object System.Drawing.Bitmap(3200, 1000)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    try {
        $graphics.Clear([System.Drawing.Color]::FromArgb(5, 15, 28))
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        Add-FittedImage $graphics $left 0 1600 1000 "APPROVED HTML BASELINE"
        Add-FittedImage $graphics $right 1600 1600 1000 "MS-04 NATIVE QT"
        $graphics.DrawLine([System.Drawing.Pens]::DarkSlateGray, 1599, 0, 1599, 999)
        $target = Join-Path $OutputDirectory "$Name.png"
        $canvas.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $canvas.Dispose()
        $left.Dispose()
        $right.Dispose()
    }
}

$comparisons = @(
    @("MS-04_P01_side-by-side", "A01_", "MS-04_P01_1920x1080.png"),
    @("MS-04_P02_side-by-side", "A02_", "MS-04_P02_1600x900.png"),
    @("MS-04_W01_side-by-side", "A03_", "MS-04_W01_1600x900.png"),
    @("MS-04_W05_side-by-side", "A04_", "MS-04_W05_1600x900.png"),
    @("MS-04_P03_side-by-side", "A06_", "MS-04_P03_1600x900.png"),
    @("MS-04_P05_side-by-side", "A08_", "MS-04_P05_1600x900.png")
)
foreach ($comparison in $comparisons) {
    New-Comparison -Name $comparison[0] -Baseline (Get-AuditScreenshot $comparison[1]) `
        -Current (Join-Path $CurrentDirectory $comparison[2])
}

Get-ChildItem -LiteralPath $OutputDirectory -Filter "MS-04_*_side-by-side.png" |
    Sort-Object Name |
    Select-Object Name, Length, LastWriteTime
