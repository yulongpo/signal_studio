$ErrorActionPreference='Stop'
Set-Location -LiteralPath 'D:/project/signal_studio_rebuild'
$taskRoot=[IO.Path]::GetFullPath('out/tasks/wideband-detection/UX-R3-FEEDBACK-01')
$statusPath=Join-Path $taskRoot 'diagnostics/remaining-status.json'
$steps=@()
function Run-Step([string]$Name,[scriptblock]$Action) {
    [ordered]@{stage=$Name;status='running';steps=$script:steps;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statusPath -Encoding utf8
    try {
        & $Action
        if(-not $?){throw "Step failed: $Name"}
        $script:steps+=@{name=$Name;pass=$true;finishedUtc=[DateTime]::UtcNow.ToString('o')}
    } catch {
        $script:steps+=@{name=$Name;pass=$false;error=$_.ToString()}
        [ordered]@{stage=$Name;status='failed';steps=$script:steps} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statusPath -Encoding utf8
        throw
    }
}
$env:SS_NARROWBAND_NODE='W8'
Run-Step 'Debug-relevant-CTest-14' {
    & scripts/build.ps1 -Configuration Debug -Action Test -TestRegex '^signal_studio_(wav|import|label_import|narrowband_analysis_ui|ui(_.*)?|ux_.*)$' -VerboseTests *> (Join-Path $taskRoot 'logs/test-debug-serial-final-complete.txt')
    if(-not $?){throw 'Debug CTest failed'}
}
foreach($run in @(@{configuration='Release';software=$true},@{configuration='Debug';software=$false},@{configuration='Debug';software=$true})) {
    $configuration=$run.configuration
    $software=$run.software
    $mode=if($software){'software'}else{'gpu'}
    Run-Step "$configuration-native-axes-label-$mode" {
        & scripts/verify-ux-r3.ps1 -Configuration $configuration -Action Native -SoftwareRenderer:$software -OutputDirectory (Join-Path $taskRoot "native-final-$configuration-$mode") *> (Join-Path $taskRoot "logs/native-final-$configuration-$mode.txt")
        if(-not $?){throw 'Native axis/label validation failed'}
    }
    Run-Step "$configuration-native-narrowband-$mode" {
        & scripts/verify-narrowband-analysis.ps1 -Configuration $configuration -Node W8 -Native -SoftwareRenderer:$software -OutputDirectory (Join-Path $taskRoot "native-final-narrowband-$configuration-$mode") *> (Join-Path $taskRoot "logs/native-final-narrowband-$configuration-$mode.txt")
        if(-not $?){throw 'Native narrowband validation failed'}
    }
}
foreach($configuration in @('Debug','Release')) {
    foreach($software in @($false,$true)) {
        foreach($narrowband in @($false,$true)) {
            $mode=if($software){'software'}else{'gpu'}
            $view=if($narrowband){'five-pages'}else{'wideband'}
            $name="package-$configuration-$view-$mode"
            Run-Step $name {
                & scripts/verify-package.ps1 -Configuration $configuration -SoftwareRenderer:$software -Narrowband:$narrowband -ReportPath (Join-Path $taskRoot "diagnostics/$name.json") *> (Join-Path $taskRoot "logs/$name.txt")
                if(-not $?){throw 'Standalone package validation failed'}
            }
        }
    }
}
[ordered]@{status='passed';stage='complete';steps=$steps;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statusPath -Encoding utf8
