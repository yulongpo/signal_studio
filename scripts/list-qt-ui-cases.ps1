[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug',[switch]$Freeze)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repoRoot 'out/tasks/wideband-detection/NB-W8/assets/ui-cases'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$savedPlatform=$env:QT_QPA_PLATFORM
try {
    $env:QT_QPA_PLATFORM='offscreen'
    foreach($target in @('SignalStudioUiTests','SignalStudioNarrowbandAnalysisUiTests')) {
        $executable=Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/$target.exe"
        & $executable '-datatags' 2>&1 | Out-File -LiteralPath (Join-Path $output "$target.txt") -Encoding utf8
        if($LASTEXITCODE -ne 0){throw "Cannot enumerate $target data tags"}
        if($Freeze) {
            $name=if($target -eq 'SignalStudioUiTests'){'ui_main_cases.txt'}else{'ui_narrowband_cases.txt'}
            $expected=if($target -eq 'SignalStudioUiTests'){106}else{9}
            $lines=@(Get-Content -LiteralPath (Join-Path $output "$target.txt") | Where-Object {$_ -match '^\w+ \w+( .+)?$'})
            if($lines.Count -ne $expected){throw "Unexpected $target inventory count $($lines.Count), expected $expected"}
            $lines | Set-Content -LiteralPath (Join-Path $repoRoot "tests/fixtures/$name") -Encoding utf8
        }
    }
} finally {$env:QT_QPA_PLATFORM=$savedPlatform}
Write-Host "Qt-generated case inventories: $output"
