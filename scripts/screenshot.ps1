[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string[]]$Sizes = @('2560x1440'),
    [string]$OutputDirectory,
    [string]$Project,
    [switch]$Narrowband,
    [switch]$SoftwareRenderer
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'artifacts/screenshots' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($Project) { $Project = [IO.Path]::GetFullPath($Project) }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$outputDir = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$executable = Join-Path $outputDir 'SignalStudio.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable does not exist: $executable" }
if ($Narrowband -and $Project) { throw 'The packaged narrowband demo cannot be combined with -Project.' }
$savedPlatform = [Environment]::GetEnvironmentVariable('QT_QPA_PLATFORM', 'Process')
$savedScale = [Environment]::GetEnvironmentVariable('QT_SCALE_FACTOR', 'Process')
$savedScreenScale = [Environment]::GetEnvironmentVariable('QT_SCREEN_SCALE_FACTORS', 'Process')
$renderMode = if ($SoftwareRenderer) { 'software' } else { 'gpu' }
$renderOption = if ($SoftwareRenderer) { '--software-renderer' } else { '--require-gpu' }
Push-Location -LiteralPath $outputDir
try {
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_SCALE_FACTOR = $null
    $env:QT_SCREEN_SCALE_FACTORS = $null
    foreach ($size in $Sizes) {
        if ($size -notmatch '^\d+x\d+$') { throw "Invalid window size: $size" }
        $prefix = if ($Narrowband) { 'signal-studio-narrowband' } else { 'signal-studio' }
        $screenshotPath = Join-Path $OutputDirectory "$prefix-$($Configuration.ToLowerInvariant())-$renderMode-$size.png"
        $reportPath = Join-Path $OutputDirectory "$prefix-$($Configuration.ToLowerInvariant())-$renderMode-$size.json"
        $appArguments = @()
        if ($Narrowband) { $appArguments += @('--narrowband-demo', '--verify-narrowband-pages') } else { $appArguments += '--demo-data' }
        $appArguments += @('--screenshot', $screenshotPath, '--size', $size, '--screen', '2', '--full-screen', '--render-report', $reportPath, $renderOption)
        if ($size -eq '2560x1440') { $appArguments += '--verify-4k-150' }
        if ($Project) { $appArguments += @('--project', $Project) }
        & $executable @appArguments | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Screenshot failed with exit code $LASTEXITCODE" }
        if (-not (Test-Path -LiteralPath $screenshotPath) -or (Get-Item -LiteralPath $screenshotPath).Length -eq 0) {
            throw "Screenshot was not produced: $screenshotPath"
        }
        Write-Host "Screenshot saved ($renderMode / Windows platform): $screenshotPath"
    }
}
finally {
    [Environment]::SetEnvironmentVariable('QT_QPA_PLATFORM', $savedPlatform, 'Process')
    [Environment]::SetEnvironmentVariable('QT_SCALE_FACTOR', $savedScale, 'Process')
    [Environment]::SetEnvironmentVariable('QT_SCREEN_SCALE_FACTORS', $savedScreenScale, 'Process')
    Pop-Location
}
