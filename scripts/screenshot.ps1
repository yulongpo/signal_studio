[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string[]]$Sizes = @('1920x1080', '1366x768'),
    [string]$OutputDirectory,
    [string]$Project,
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
$savedPlatform = [Environment]::GetEnvironmentVariable('QT_QPA_PLATFORM', 'Process')
$renderMode = if ($SoftwareRenderer) { 'software' } else { 'gpu' }
$renderOption = if ($SoftwareRenderer) { '--software-renderer' } else { '--require-gpu' }
Push-Location -LiteralPath $outputDir
try {
    $env:QT_QPA_PLATFORM = 'windows'
    foreach ($size in $Sizes) {
        if ($size -notmatch '^\d+x\d+$') { throw "Invalid window size: $size" }
        $screenshotPath = Join-Path $OutputDirectory "signal-studio-$($Configuration.ToLowerInvariant())-$renderMode-$size.png"
        $appArguments = @('--screenshot', $screenshotPath, '--size', $size, $renderOption)
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
    Pop-Location
}
