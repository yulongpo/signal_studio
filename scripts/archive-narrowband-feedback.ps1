[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$task=Join-Path $repo 'out/tasks/wideband-detection/NB-FEEDBACK-01'
$destination=Join-Path $repo 'docs/implementation/narrowband-constellation-eye-v2/feedback-2026-10-23'
foreach($part in @('evidence','assets')){New-Item -ItemType Directory -Path (Join-Path $destination $part) -Force|Out-Null}
$summary=[ordered]@{generated=(Get-Date -Format o);fullSuiteRerun=$false;currentCTestInventory=37;configurations=@()}
foreach($configuration in @('Debug','Release')) {
    $lower=$configuration.ToLowerInvariant()
    $logs=Get-ChildItem (Join-Path $task 'logs') -Filter "directed-*-$configuration.txt"|Where-Object {
        $text=Get-Content $_.FullName -Raw
        $text -match '100% tests passed, 0 tests failed out of 6' -and $text -match "Test project [^\r\n]*vs2026-qt611-$lower"
    }|Sort-Object LastWriteTime
    $log=$logs|Select-Object -Last 1;if(-not $log){throw "Missing complete directed six-test $configuration log"}
    Copy-Item $log.FullName (Join-Path $destination "evidence/$lower-directed.txt")
    $numeric=Get-ChildItem (Join-Path $task 'diagnostics') -Recurse -Filter 'modulation.json'|Where-Object {$_.DirectoryName -match "-$configuration$"}|Sort-Object LastWriteTime|Select-Object -Last 1
    $values=@(Get-Content $numeric.FullName -Raw|ConvertFrom-Json)
    if($values.Count -ne 50 -or @($values|Where-Object {-not $_.pass -or $_.truthEvm -ge $_.truthEvmLimit}).Count){throw "Incomplete $configuration 50-case numerical matrix"}
    Copy-Item $numeric.FullName (Join-Path $destination "evidence/$lower-modulation.json")
    foreach($name in @('recovery','symbol-rate')){Copy-Item (Join-Path $numeric.DirectoryName "$name.json") (Join-Path $destination "evidence/$lower-$name.json")}
    $qt=Join-Path $repo "out/vs2026-qt611-$lower/nb-analysis-ui-$lower.txt"
    $text=Get-Content $qt -Raw
    if($text -notmatch 'Totals: 11 passed, 0 failed, 0 skipped' -or $text -notmatch 'NARROWBAND_VALIDATION_NODE=W8'){throw 'Incomplete expanded Qt regression'}
    Copy-Item $qt (Join-Path $destination "evidence/$lower-narrowband-ui.txt")
    $parameters=Join-Path $task "logs/parameter-regression-$lower.txt"
    if((Get-Content $parameters -Raw) -notmatch '100% tests passed, 0 tests failed out of 4'){throw 'Parameter/layout/popover regression not complete'}
    Copy-Item $parameters (Join-Path $destination "evidence/$lower-parameter-regression.txt")
    $input=Get-ChildItem (Join-Path $task 'logs') -Filter "input-native-*-$configuration.txt"|Sort-Object LastWriteTime|Select-Object -Last 1
    if(-not $input -or (Get-Content $input.FullName -Raw) -notmatch 'Totals: 5 passed, 0 failed, 0 skipped'){throw 'Missing Windows input event unit test'}
    Copy-Item $input.FullName (Join-Path $destination "evidence/$lower-native-input.txt")
    $probe=Get-ChildItem (Join-Path $task 'diagnostics') -Filter "user-source-*-$configuration.json"|Sort-Object LastWriteTime|Select-Object -Last 1
    $probeReport=Get-Content $probe.FullName -Raw|ConvertFrom-Json
    if(-not $probeReport.pass){throw 'Actual source reproduction failed'}
    Copy-Item $probe.FullName (Join-Path $destination "evidence/$lower-user-source.json")
    $native=@()
    foreach($software in @($false,$true)) {
        $mode=if($software){'software'}else{'gpu'}
        $exe=Get-Item (Join-Path $repo "out/vs2026-qt611-$lower`_bin/SignalStudioUiCapture.exe")
        $reports=foreach($file in (Get-ChildItem (Join-Path $task 'diagnostics') -Recurse -Filter 'feedback-report.json')) {
            $r=Get-Content $file.FullName -Raw|ConvertFrom-Json
            if($r.configuration -eq $configuration -and $r.softwareRenderer -eq $software -and $r.pass -and $r.nativeAcceptance -and $r.layoutUnified -and $r.sceneCount -eq 7 -and $file.LastWriteTime -ge $exe.LastWriteTime){[pscustomobject]@{file=$file;report=$r}}
        }
        $chosen=$reports|Sort-Object {$_.file.LastWriteTime}|Select-Object -Last 1
        if(-not $chosen){throw "Missing final-binary $configuration $mode actual monitor evidence"}
        Copy-Item $chosen.file.FullName (Join-Path $destination "evidence/$lower-native-$mode.json")
        if($configuration -eq 'Release') {
            $image=$chosen.report.scenes|Where-Object name -EQ '03-32qam-recovered'
            Copy-Item $image.png (Join-Path $destination "assets/release-$mode-32qam.png")
            foreach($sceneName in @('04-eye-settings','05-constellation-settings')) {
                $image=$chosen.report.scenes|Where-Object name -EQ $sceneName
                Copy-Item $image.png (Join-Path $destination "assets/release-$mode-$sceneName.png")
            }
        }
        $package=Join-Path $task "diagnostics/package-$lower-$mode-final.json"
        $packageReport=Get-Content $package -Raw|ConvertFrom-Json
        if(-not $packageReport.pass -or -not $packageReport.analysis.carrierLocked -or -not $packageReport.standaloneProcess.systemOnlyPath){throw 'Final package evidence incomplete'}
        Copy-Item $package (Join-Path $destination "evidence/$lower-package-$mode.json")
        $native += [ordered]@{mode=$mode;scenes=7;layoutUnified=$true;report=$chosen.file.FullName;screen=$chosen.report.screen}
    }
    $summary.configurations += [ordered]@{configuration=$configuration;targetedCTest=6;parameterRegressionCTest=4;positiveModulations=50;noiseHypotheses=6;uiPass=11;nativeInputPass=5;native=$native;userSource=$probeReport.runs}
}
$summary|ConvertTo-Json -Depth 40|Set-Content (Join-Path $destination 'evidence/SUMMARY.json') -Encoding utf8
Copy-Item (Join-Path $task 'logs/test-inventory.txt') (Join-Path $destination 'evidence/test-inventory.txt')
$failures=Get-ChildItem (Join-Path $task 'logs') -Filter '*.txt'|Where-Object {
    $_.Name -eq 'parameter-regression-debug-invalid-method.txt' -or
    (Get-Content $_.FullName -Raw) -match '(?m)^0% tests passed|^FAIL!\s*:|error C\d{4}'
}
foreach($failure in $failures){Copy-Item $failure.FullName (Join-Path $destination "evidence/failed-$($failure.Name)")}
Write-Host "Feedback evidence archived: $destination"
