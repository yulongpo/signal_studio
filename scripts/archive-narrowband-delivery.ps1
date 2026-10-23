[CmdletBinding()]
param([string]$DebugUiRetestLog,[string[]]$DebugRetestLogs=@(),[string[]]$ReleaseRetestLogs=@(),[switch]$RecordGitIdentity,[switch]$VerifyCoverageGate,[switch]$VerifyRetestGate)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$taskRoot = Join-Path $repoRoot 'out/tasks/wideband-detection/NB-W8'
$destination = Join-Path $repoRoot 'docs/implementation/narrowband-constellation-eye-v2'
function Assert-RetestLog([string]$Text,[string]$Configuration,[string]$Path) {
    if($Text -notmatch '100% tests passed, 0 tests failed out of \d+') {throw "Retest is not successful: $Path"}
    # Use CTest's actual build directory rather than a caller-supplied filename.
    $projects=@([regex]::Matches($Text,'(?im)^Test project [^\r\n]*[\\/]vs2026-qt611-(debug|release)\s*$') |
        ForEach-Object {$_.Groups[1].Value.ToLowerInvariant()})
    if($projects.Count -ne 1 -or $projects[0] -ne $Configuration.ToLowerInvariant()) {
        throw "RETEST_CONFIGURATION_MISMATCH: expected $Configuration in $Path"
    }
}
function Assert-QtCoverage([string[]]$LogPaths,[string]$Inventory,[switch]$Narrowband) {
    $expected = @(Get-Content -LiteralPath (Join-Path $repoRoot "tests/fixtures/$Inventory") | ForEach-Object {
        $parts = $_ -split ' ',3
        if($parts.Count -lt 2){throw 'Invalid Qt case inventory'}
        $tag = if($parts.Count -gt 2){$parts[2]}else{''}
        "$($parts[0])::$($parts[1])($tag)"
    })
    $rows=@();$passed=0
    foreach($path in $LogPaths) {
        $log=Get-Content -LiteralPath $path -Raw
        $totals=[regex]::Match($log,'Totals: (\d+) passed, 0 failed, 0 skipped')
        if(-not $totals.Success -or $log -match '(?m)^(FAIL!|XPASS|QFATAL|SKIP)\s*:'){throw "Incomplete Qt log: $path"}
        if($Narrowband -and $log -notmatch 'NARROWBAND_VALIDATION_NODE=W8'){throw 'Final narrowband UI must execute W8 assertions'}
        $passRows=@([regex]::Matches($log,'(?m)^PASS\s+:\s+(\w+::[^\r\n]+)') | ForEach-Object {$_.Groups[1].Value.Trim()})
        if($passRows.Count -ne [int]$totals.Groups[1].Value){throw "PASS rows disagree with totals: $path"}
        $passed += $passRows.Count
        $rows += $passRows | Where-Object {$_ -notmatch '::(initTestCase|cleanupTestCase)\(\)$'}
    }
    $unique=@($rows | Sort-Object -Unique -CaseSensitive)
    if($rows.Count -ne $expected.Count -or $unique.Count -ne $expected.Count -or
       @(Compare-Object -ReferenceObject $expected -DifferenceObject $unique -CaseSensitive).Count -ne 0){throw "UI_CASE_COVERAGE_MISMATCH: expected $($expected.Count), observed $($rows.Count), unique $($unique.Count)"}
    return [pscustomobject]@{businessCases=$unique.Count;totalPassed=$passed;logs=$LogPaths}
}
if($VerifyCoverageGate) {
    $full=Join-Path $repoRoot 'out/vs2026-qt611-release/ui-test-results-release.txt'
    $accepted=Assert-QtCoverage @($full) 'ui_main_cases.txt'
    $probeRoot=Join-Path $taskRoot 'diagnostics/evidence-gate'
    New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
    $selected=Join-Path $probeRoot 'selected-fixture.txt'
    $fixture=@(Get-Content -LiteralPath $full | Where-Object {$_ -match '^PASS\s+:\s+UiTests::(initTestCase|workspaceLayout|cleanupTestCase)\(\)$'})
    $fixture += 'Totals: 3 passed, 0 failed, 0 skipped'
    $fixture | Set-Content -LiteralPath $selected -Encoding utf8
    try {$null=Assert-QtCoverage @($selected) 'ui_main_cases.txt';throw 'Selected UI cases were incorrectly accepted'}
    catch {if($_.Exception.Message -notmatch '^UI_CASE_COVERAGE_MISMATCH:'){throw}}
    [ordered]@{acceptedComplete=$true;completeBusinessCases=$accepted.businessCases;rejectedSelected=$true;fixturePurpose='evidence gate unit test only'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $probeRoot 'gate-check.json') -Encoding utf8
    Write-Host 'Complete case inventory accepted; selected-case log rejected.'
    return
}
if($VerifyRetestGate) {
    if($DebugRetestLogs.Count -eq 0 -or $ReleaseRetestLogs.Count -eq 0){throw 'Provide both actual Debug and Release passing retest logs'}
    foreach($configuration in @('Debug','Release')) {
        $paths=if($configuration -eq 'Debug'){$DebugRetestLogs}else{$ReleaseRetestLogs}
        $opposite=if($configuration -eq 'Debug'){'Release'}else{'Debug'}
        foreach($path in $paths) {
            $log=Get-Content -LiteralPath $path -Raw
            Assert-RetestLog $log $configuration $path
            try {Assert-RetestLog $log $opposite $path;throw 'Wrong configuration was incorrectly accepted'}
            catch {if($_.Exception.Message -notmatch '^RETEST_CONFIGURATION_MISMATCH:'){throw}}
        }
    }
    $probeRoot=Join-Path $taskRoot 'diagnostics/evidence-gate'
    New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
    [ordered]@{acceptedMatching=$true;rejectedDebugAsRelease=$true;rejectedReleaseAsDebug=$true;fixturePurpose='evidence gate unit test with actual CTest logs'} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $probeRoot 'retest-configuration-gate.json') -Encoding utf8
    Write-Host 'Matching configurations accepted; cross-configuration retest logs rejected.'
    return
}
if ($RecordGitIdentity) {
    Push-Location -LiteralPath $repoRoot
    try {
        $identity = & git rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw 'Cannot record final commit identity' }
        $state = & git status --short
        @("commit: $identity",'branch: feature/narrowband-sampling-constellation-eye','remote actions: none','worktree:', $state) | Set-Content -LiteralPath (Join-Path $taskRoot 'diagnostics/FINAL_GIT_IDENTITY.txt') -Encoding utf8
        Write-Host "Final local commit: $identity"
    } finally { Pop-Location }
    return
}
foreach ($folder in @('evidence','assets')) { New-Item -ItemType Directory -Path (Join-Path $destination $folder) -Force | Out-Null }
$summary = [ordered]@{generated=(Get-Date -Format o);fixtureKind='independent synthetic transmitter, actual IQ files';configurations=@()}
foreach ($configuration in @('Debug','Release')) {
    $lower = $configuration.ToLowerInvariant()
    $fullLog = Get-ChildItem -LiteralPath (Join-Path $taskRoot 'logs') -Filter "final-*-$configuration.txt" | Where-Object { $_.Name -match "^final-\d{8}-\d{6}-\d{3}-$configuration\.txt$" } | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $fullLog) { throw "No full $configuration regression log" }
    $text = Get-Content -LiteralPath $fullLog.FullName -Raw
    $fullPass = $text -match '100% tests passed, 0 tests failed out of 35'
    $retestedFailures=@()
    if ($text -notmatch 'tests failed out of 35') { throw "$configuration log does not cover the full 35-test regression" }
    if (-not $fullPass) {
        $retestLogs=if($configuration -eq 'Debug'){@($DebugRetestLogs)}else{@($ReleaseRetestLogs)}
        if($configuration -eq 'Debug' -and $DebugUiRetestLog){$retestLogs+= $DebugUiRetestLog}
        $failed = @([regex]::Matches($text,'(?m)^\s*\d+\s*-\s*(signal_studio_\w+)\s*\(') | ForEach-Object { $_.Groups[1].Value })
        if($failed.Count -eq 0 -or $retestLogs.Count -eq 0){throw "$configuration regression failures have no explicit retest evidence"}
        $retryTexts=@();$index=0
        foreach($path in $retestLogs) {
            $retry=Get-Content -LiteralPath $path -Raw
            Assert-RetestLog $retry $configuration $path
            $retryTexts+=$retry;Copy-Item -LiteralPath $path -Destination (Join-Path $destination "evidence/$lower-retest-$index.txt");++$index
        }
        foreach($name in $failed) {
            $pattern='Test\s+#\d+:\s+'+[regex]::Escape($name)+'\s+\.+\s+Passed'
            if(@($retryTexts|Where-Object {$_ -match $pattern}).Count -eq 0){throw "Missing passing retest for $name"}
            $retestedFailures+=$name
        }
    }
    Copy-Item -LiteralPath $fullLog.FullName -Destination (Join-Path $destination "evidence/$lower-full-regression.txt")
    $mainLogs=@('geometry','parameters','annotations' | ForEach-Object {Join-Path $repoRoot "out/vs2026-qt611-$lower/ui-$_-test-results-$lower.txt"})
    $mainCoverage=Assert-QtCoverage $mainLogs 'ui_main_cases.txt'
    $narrowLog=Join-Path $repoRoot "out/vs2026-qt611-$lower/nb-analysis-ui-$lower.txt"
    $narrowCoverage=Assert-QtCoverage @($narrowLog) 'ui_narrowband_cases.txt' -Narrowband
    foreach($file in $mainLogs+@($narrowLog)) {Copy-Item -LiteralPath $file -Destination (Join-Path $destination "evidence/$lower-$([IO.Path]::GetFileName($file))")}
    $config = [ordered]@{configuration=$configuration;fullRegressionOneRunPassed=$fullPass;retestedFailures=$retestedFailures;verifiedCTestCount=35;mainUi=$mainCoverage;narrowbandUi=$narrowCoverage;native=@()}
    foreach ($software in @($false,$true)) {
        $reports = foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $taskRoot 'diagnostics') -Recurse -Filter 'narrowband-analysis-report.json')) {
            $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if ($report.process.configuration -eq $configuration -and $report.softwareRenderer -eq $software -and $report.pass -and $report.nativeAcceptance -and $report.sceneCount -eq 12) { [pscustomobject]@{file=$file;report=$report} }
        }
        $chosen = $reports | Sort-Object { $_.file.LastWriteTime } | Select-Object -Last 1
        if (-not $chosen) { throw "Missing $configuration software=$software physical monitor acceptance" }
        $mode = if ($software) { 'software' } else { 'gpu' }
        Copy-Item -LiteralPath $chosen.file.FullName -Destination (Join-Path $destination "evidence/$lower-native-$mode.json")
        $config.native += [ordered]@{mode=$mode;sceneCount=12;screen=$chosen.report.screen;report=$chosen.file.FullName}
        if ($configuration -eq 'Release') {
            $scene = $chosen.report.scenes | Where-Object scene -EQ 'cf32-04-recovered'
            Copy-Item -LiteralPath $scene.png -Destination (Join-Path $destination "assets/release-$mode-recovered.png")
        }
        $package = Get-ChildItem -LiteralPath (Join-Path $taskRoot 'diagnostics') -Filter "package-$lower-$mode*.json" | Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $package) { throw "Missing standalone $configuration $mode package check" }
        $packageReport = Get-Content -LiteralPath $package.FullName -Raw | ConvertFrom-Json
        if (-not $packageReport.pass -or -not $packageReport.analysis.carrierLocked -or -not $packageReport.standaloneProcess.systemOnlyPath) { throw 'Incomplete standalone real analysis evidence' }
        Copy-Item -LiteralPath $package.FullName -Destination (Join-Path $destination "evidence/$lower-package-$mode.json")
    }
    $summary.configurations += $config
    foreach ($name in @('recovery','symbol-rate')) {
        $report = Get-ChildItem -LiteralPath (Join-Path $taskRoot 'diagnostics') -Recurse -Filter "$name.json" | Where-Object { $_.DirectoryName -match $configuration } | Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $report) { throw "Missing final $configuration $name numerical report" }
        Copy-Item -LiteralPath $report.FullName -Destination (Join-Path $destination "evidence/$lower-$name.json")
    }
}
foreach($name in @('final-numeric-observations-debug.txt','final-review-recovery-debug.txt','final-review-race-debug.txt','final-review-race-release.txt')) {
    $extra=Join-Path $taskRoot "logs/$name"
    if(Test-Path -LiteralPath $extra){Copy-Item -LiteralPath $extra -Destination (Join-Path $destination "evidence/$name")}
}
$gate=Join-Path $taskRoot 'diagnostics/evidence-gate/gate-check.json'
if(Test-Path -LiteralPath $gate){Copy-Item -LiteralPath $gate -Destination (Join-Path $destination 'evidence/ui-coverage-gate.json')}
$retestGate=Join-Path $taskRoot 'diagnostics/evidence-gate/retest-configuration-gate.json'
if(Test-Path -LiteralPath $retestGate){Copy-Item -LiteralPath $retestGate -Destination (Join-Path $destination 'evidence/retest-configuration-gate.json')}
$legacy=Get-ChildItem -LiteralPath (Join-Path $taskRoot 'logs') -Filter 'final-*-Debug.txt' | Where-Object {$_.Name -match '^final-\d{8}-\d{6}-\d{3}-Debug\.txt$'} | Sort-Object LastWriteTime | Select-Object -First 1
if($legacy -and (Get-Content $legacy.FullName -Raw) -match 'signal_studio_ui \(Timeout\)'){Copy-Item -LiteralPath $legacy.FullName -Destination (Join-Path $destination 'evidence/legacy-monolithic-ui-timeout.txt')}
$summary | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath (Join-Path $destination 'evidence/SUMMARY.json') -Encoding utf8
Write-Host "Archived validation evidence to $destination without adding SHA manifests."
