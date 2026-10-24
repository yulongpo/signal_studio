[CmdletBinding()]
param([switch]$Push,[string]$ReviewReport)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if(-not $ReviewReport){$ReviewReport=Join-Path $repoRoot 'docs/implementation/ux-r3/INDEPENDENT_REVIEW.md'}
Push-Location -LiteralPath $repoRoot
try{
    $report=Get-Content -LiteralPath $ReviewReport -Raw
    $decisions=@([regex]::Matches($report,'(?m)^Decision:\s*(APPROVED|REJECTED|BLOCKED)\s*$'))
    $heads=@([regex]::Matches($report,'(?m)^Reviewed-HEAD:\s*([0-9a-f]{40})\s*$'))
    if($decisions.Count -ne 1 -or $decisions[0].Groups[1].Value -ne 'APPROVED' -or $heads.Count -ne 1){throw 'Final independent review must explicitly approve exactly one HEAD'}
    $approved=$heads[0].Groups[1].Value
    $head=(& git rev-parse HEAD).Trim()
    if($LASTEXITCODE -ne 0 -or $head -ne $approved){throw 'Current HEAD differs from independent approved HEAD; re-review required'}
    & git diff --quiet
    if($LASTEXITCODE -ne 0){throw 'Tracked working-tree changes exist; commit and re-review required'}
    & git diff --cached --quiet
    if($LASTEXITCODE -ne 0){throw 'Staged changes exist; commit and re-review required'}
    & git fetch origin
    if($LASTEXITCODE -ne 0){throw 'Remote fetch failed; no publication'}
    & git merge-base --is-ancestor origin/rebuild/a143-native $approved
    if($LASTEXITCODE -ne 0){throw 'Remote contains new/divergent commits: safely integrate, retest and re-review before publication'}
    $remoteBefore=(& git rev-parse origin/rebuild/a143-native).Trim()
    if($Push){
        & git push origin "${approved}:refs/heads/rebuild/a143-native"
        if($LASTEXITCODE -ne 0){throw 'Normal push rejected; no force permitted'}
        $remote=(& git ls-remote origin refs/heads/rebuild/a143-native)
        if($LASTEXITCODE -ne 0 -or -not $remote.StartsWith($approved)){throw 'Remote final HEAD verification failed'}
        Write-Output "Published approved HEAD $approved; remote previous $remoteBefore; target rebuild/a143-native"
    }else{Write-Output "Local gate passed for $approved; remote $remoteBefore; no push requested"}
}finally{Pop-Location}
