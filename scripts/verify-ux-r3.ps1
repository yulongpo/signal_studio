[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [ValidateSet('BuildTest','Native','Package','Probe','Association')][string]$Action='BuildTest',
    [switch]$SoftwareRenderer,[int]$Jobs=6,[string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repoRoot 'out/tasks/wideband-detection/UX-R3/G7'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
foreach($part in @('logs','diagnostics','assets')){New-Item -ItemType Directory -Force -Path (Join-Path $OutputDirectory $part)|Out-Null}
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$mode=if($SoftwareRenderer){'software'}else{'gpu'}
Push-Location -LiteralPath $repoRoot
try{
    if($Action -eq 'BuildTest'){
        $env:SS_UI_CASES=$null
        $env:SS_NARROWBAND_NODE='W8'
        $env:SS_NARROWBAND_REPORT_DIR=Join-Path $OutputDirectory "diagnostics/$Configuration-full-$stamp"
        New-Item -ItemType Directory -Force -Path $env:SS_NARROWBAND_REPORT_DIR | Out-Null
        & "$PSScriptRoot/build.ps1" -Configuration $Configuration -Action All -Jobs $Jobs *> (Join-Path $OutputDirectory "logs/$Configuration-full-$stamp.txt")
        if(-not $?){throw 'UX-R3 full build or CTest failed; preserve the full log'}
    }elseif($Action -eq 'Native'){
        $destination=Join-Path $OutputDirectory "diagnostics/$Configuration-$mode-$stamp"
        New-Item -ItemType Directory -Path $destination | Out-Null
        $names=@('QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS','QT_FONT_DPI','QT_DEVICE_PIXEL_RATIO','QT_AUTO_SCREEN_SCALE_FACTOR','QT_ENABLE_HIGHDPI_SCALING','QT_SCALE_FACTOR_ROUNDING_POLICY','QT_USE_PHYSICAL_DPI','QT_QPA_PLATFORM')
        $saved=@{}
        try{
            foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process');[Environment]::SetEnvironmentVariable($name,$null,'Process')}
            $env:QT_QPA_PLATFORM='windows'
            $exe=Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioUiCapture.exe"
            $arguments=@('--ux-r3',$destination)
            if($SoftwareRenderer){$arguments+='--software-renderer'}
            & $exe @arguments *> (Join-Path $OutputDirectory "logs/$Configuration-native-$mode-$stamp.txt")
            if($LASTEXITCODE -ne 0){throw 'UX-R3 native capture failed; inspect native-report.json and console log'}
            $runs=@(Get-ChildItem -LiteralPath $destination -Directory -Filter 'ux-r3-*')
            if($runs.Count -ne 1){throw 'Native runner did not produce exactly one isolated run'}
            $report=Get-Content -LiteralPath (Join-Path $runs[0].FullName 'ux-r3-report.json') -Raw | ConvertFrom-Json
            if($report.pass -ne $true){throw 'Native report did not pass'}
        }finally{foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}
    }elseif($Action -eq 'Package'){
        & "$PSScriptRoot/verify-package.ps1" -Configuration $Configuration -SoftwareRenderer:$SoftwareRenderer -ReportPath (Join-Path $OutputDirectory "diagnostics/package-$Configuration-$mode-$stamp.json") *> (Join-Path $OutputDirectory "logs/package-$Configuration-$mode-$stamp.txt")
        if(-not $?){throw 'Standalone package validation failed'}
    }elseif($Action -eq 'Probe'){
        & "$PSScriptRoot/verify-narrowband-analysis.ps1" -Node W8 -Configuration $Configuration -ProbeScreens -OutputDirectory $OutputDirectory *> (Join-Path $OutputDirectory "diagnostics/screens-$Configuration-$stamp.json")
        if(-not $?){throw 'Native screen inventory failed'}
    }else{
        & "$PSScriptRoot/ssproj-association.ps1" -WhatIf *> (Join-Path $OutputDirectory "logs/association-$stamp.txt")
    }
}finally{Pop-Location}
