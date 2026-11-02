[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug',
      [Parameter(Mandatory)][string]$ReportPath)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
$lower=$Configuration.ToLowerInvariant()
$build=Join-Path $repoRoot "out/vs2026-qt611-$lower"
$summary=[ordered]@{schemaVersion=1;configuration=$Configuration;pass=$false;groups=@()}
foreach($suite in @('main','narrowband')){
    $inventory=Join-Path $repoRoot "tests/fixtures/ui_${suite}_cases.txt"
    $expected=@(Get-Content -LiteralPath $inventory | ForEach-Object {
        $parts=$_ -split ' ',3
        if($parts.Count -lt 2){throw "Invalid Qt inventory: $_"}
        $tag=if($parts.Count -eq 3){$parts[2]}else{''}
        "$($parts[0])::$($parts[1])($tag)"
    })
    $logs=if($suite -eq 'main'){@('geometry','parameters','annotations'|ForEach-Object {Join-Path $build "ui-$_-test-results-$lower.txt"})}else{@(Join-Path $build "nb-analysis-ui-$lower.txt")}
    $rows=@()
    foreach($path in $logs){
        $raw=Get-Content -LiteralPath $path -Raw
        $totals=[regex]::Match($raw,'Totals: (\d+) passed, 0 failed, 0 skipped')
        if(-not $totals.Success -or $raw -match '(?m)^(FAIL!|XPASS|QFATAL|SKIP)\s*:'){throw "Incomplete or failed Qt log: $path"}
        if($suite -eq 'narrowband' -and $raw -notmatch 'NARROWBAND_VALIDATION_NODE=W8'){throw 'Narrowband final coverage must execute W8 assertions'}
        $passRows=@([regex]::Matches($raw,'(?m)^PASS\s+:\s+(\w+::[^\r\n]+)')|ForEach-Object {$_.Groups[1].Value.Trim()})
        if($passRows.Count -ne [int]$totals.Groups[1].Value){throw "Qt totals disagree with rows: $path"}
        $rows+=@($passRows|Where-Object {$_ -notmatch '::(initTestCase|cleanupTestCase)\(\)$'})
    }
    $unique=@($rows|Sort-Object -Unique -CaseSensitive)
    if($rows.Count -ne $expected.Count -or $unique.Count -ne $expected.Count -or
       @(Compare-Object -ReferenceObject $expected -DifferenceObject $unique -CaseSensitive).Count -ne 0){throw "Qt coverage mismatch: suite=$suite expected=$($expected.Count) rows=$($rows.Count) unique=$($unique.Count)"}
    $summary.groups+=@{suite=$suite;businessCases=$rows.Count;logs=$logs;inventory=$inventory}
}
$summary.pass=$true
$destination=[IO.Path]::GetFullPath($ReportPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination)|Out-Null
$summary|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $destination -Encoding utf8
Write-Output "Full Qt row coverage passed: $Configuration"
