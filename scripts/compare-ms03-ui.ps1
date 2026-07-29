[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Baseline,
    [Parameter(Mandatory = $true)][string]$After,
    [string]$Before = '',
    [Parameter(Mandatory = $true)][string]$Output
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepositoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $repositoryRoot $Path)).Path
}

$baselinePath = Resolve-RepositoryPath $Baseline
$afterPath = Resolve-RepositoryPath $After
$beforePath = if ($Before) { Resolve-RepositoryPath $Before } else { $null }
$outputPath = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $repositoryRoot $Output }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath) | Out-Null

$images = [Collections.Generic.List[System.Drawing.Image]]::new()
$labels = [Collections.Generic.List[string]]::new()
try {
    $baselineImage = [System.Drawing.Image]::FromFile($baselinePath)
    $images.Add($baselineImage)
    $labels.Add('HTML 基线')
    if ($beforePath) {
        $images.Add([System.Drawing.Image]::FromFile($beforePath))
        $labels.Add('修复前 Qt')
    }
    $images.Add([System.Drawing.Image]::FromFile($afterPath))
    $labels.Add('修复后 Qt')

    $panelWidth = $baselineImage.Width
    $panelHeight = $baselineImage.Height
    $labelHeight = 42
    $canvas = [System.Drawing.Bitmap]::new($panelWidth * $images.Count, $panelHeight + $labelHeight)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    try {
        $graphics.Clear([System.Drawing.Color]::FromArgb(8, 17, 31))
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $font = [System.Drawing.Font]::new('Microsoft YaHei UI', 14, [System.Drawing.FontStyle]::Bold)
        $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(229, 241, 255))
        $border = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(43, 72, 103), 1)
        try {
            for ($index = 0; $index -lt $images.Count; $index++) {
                $x = $index * $panelWidth
                $graphics.DrawString($labels[$index], $font, $brush, $x + 12, 9)
                $graphics.DrawImage($images[$index], $x, $labelHeight, $panelWidth, $panelHeight)
                $graphics.DrawRectangle($border, $x, $labelHeight, $panelWidth - 1, $panelHeight - 1)
            }
        }
        finally {
            $font.Dispose()
            $brush.Dispose()
            $border.Dispose()
        }
        $canvas.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $canvas.Dispose()
    }
}
finally {
    foreach ($image in $images) {
        $image.Dispose()
    }
}

Write-Output "已生成对比图：$outputPath"
