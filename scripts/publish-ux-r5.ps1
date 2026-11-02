[CmdletBinding()]
param([switch]$Push)
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $reviewPath = 'out/tasks/ux-r5/INDEPENDENT_REVIEW.md'
    if (-not (Test-Path -LiteralPath $reviewPath)) { throw 'UX-R5 independent review is missing' }
    $reviewText = Get-Content -LiteralPath $reviewPath -Raw
    if ($reviewText -notmatch '(?m)^Decision:\s*APPROVED\s*$') { throw 'UX-R5 independent review did not approve' }
    if ($reviewText -notmatch '(?m)^Reviewed-HEAD:\s*([0-9a-f]{40})\s*$') { throw 'Reviewed-HEAD missing' }
    $approvedRevision = $Matches[1]
    $localRevision = (git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $localRevision -ne $approvedRevision) { throw 'HEAD differs from independently reviewed version' }
    if ((git branch --show-current).Trim() -ne 'rebuild/a143-native') { throw 'Wrong target branch' }
    git diff --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Tracked worktree is dirty' }
    git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Index is dirty' }
    git fetch origin rebuild/a143-native
    if ($LASTEXITCODE -ne 0) { throw 'Fetch failed' }
    git merge-base --is-ancestor origin/rebuild/a143-native $approvedRevision
    if ($LASTEXITCODE -ne 0) { throw 'Remote has unintegrated commits: integrate, test, and independently review the new HEAD' }
    if ((git rev-parse HEAD).Trim() -ne $approvedRevision) { throw 'HEAD changed during publication checks; re-review required' }
    Write-Host "UX-R5 exact independently approved HEAD: $approvedRevision; remote ancestry verified."
    if (-not $Push) { return }
    # Pin the source ref to the reviewed commit. A concurrent remote advance is
    # rejected by this normal, non-force push.
    git push origin "${approvedRevision}:refs/heads/rebuild/a143-native"
    if ($LASTEXITCODE -ne 0) { throw 'Normal push failed; no force retry permitted' }
    $remoteLine = git ls-remote origin refs/heads/rebuild/a143-native
    if ($LASTEXITCODE -ne 0 -or ($remoteLine -split '\s+')[0] -ne $approvedRevision) { throw 'Remote HEAD verification failed' }
    [ordered]@{
        status='PUSHED_AND_VERIFIED'; head=$approvedRevision; reviewedHead=$approvedRevision;
        branch='rebuild/a143-native'; remote='origin'; normalNonForcePush=$true;
        independentReview=$reviewPath; verifiedUtc=[DateTime]::UtcNow.ToString('o')
    } | ConvertTo-Json | Set-Content -LiteralPath 'out/tasks/ux-r5/PUBLISH_RECORD.json' -Encoding utf8
} finally { Pop-Location }
