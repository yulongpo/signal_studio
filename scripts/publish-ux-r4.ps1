[CmdletBinding()]
param([switch]$Push)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $report='docs/implementation/ux-r4/INDEPENDENT_REVIEW.md'
    if(-not(Test-Path -LiteralPath $report)){throw 'Independent review is missing'}
    $review=Get-Content -LiteralPath $report -Raw
    if($review -notmatch '(?m)^Decision:\s*APPROVED\s*$'){throw 'Independent review did not approve'}
    if($review -notmatch '(?m)^Reviewed-HEAD:\s*([0-9a-f]{40})\s*$'){throw 'Reviewed-HEAD missing'}
    $approved=$Matches[1]
    $head=(git rev-parse HEAD).Trim()
    if($head -ne $approved){throw 'HEAD differs from independent reviewed HEAD'}
    if((git branch --show-current).Trim() -ne 'rebuild/a143-native'){throw 'Wrong target branch'}
    git diff --quiet
    if($LASTEXITCODE -ne 0){throw 'Tracked worktree is dirty'}
    git diff --cached --quiet
    if($LASTEXITCODE -ne 0){throw 'Index is dirty'}
    git fetch origin
    if($LASTEXITCODE -ne 0){throw 'Fetch failed'}
    git merge-base --is-ancestor origin/rebuild/a143-native HEAD
    if($LASTEXITCODE -ne 0){throw 'Remote advanced/diverged: integrate locally, test and independently review again'}
    Write-Host "Exact independently approved HEAD: $head; remote ancestry verified."
    if(-not $Push){return}
    git push origin 'HEAD:refs/heads/rebuild/a143-native'
    if($LASTEXITCODE -ne 0){throw 'Normal push failed; no force retry permitted'}
    $remote=git ls-remote origin refs/heads/rebuild/a143-native
    if($LASTEXITCODE -ne 0 -or ($remote -split '\s+')[0] -ne $approved){throw 'Remote verification failed'}
    [ordered]@{status='PUSHED_AND_VERIFIED';head=$head;reviewedHead=$approved;
        branch='rebuild/a143-native';remote='origin';normalNonForcePush=$true;
        verifiedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json |
        Set-Content -LiteralPath docs/implementation/ux-r4/PUBLISH_RECORD.json -Encoding utf8
} finally { Pop-Location }
