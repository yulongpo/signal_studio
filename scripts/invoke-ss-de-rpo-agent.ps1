[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('A','B','C','D','E','F','G','H')][string]$Role,
    [Parameter(Mandatory)][string]$Worktree,
    [Parameter(Mandatory)][string]$PromptFile,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$SessionId = [guid]::NewGuid().ToString(),
    [int]$MaxTurns = 120,
    [switch]$ReadOnly
)
$ErrorActionPreference = 'Stop'
$grok = (Get-Command grok -ErrorAction Stop).Source
$Worktree = (Get-Item -LiteralPath $Worktree).FullName
$PromptFile = (Get-Item -LiteralPath $PromptFile).FullName
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$record = [ordered]@{
    schema='signalstudio-rpo-agent-session-v1'; role=$Role; sessionId=$SessionId
    model='grok-4.7-build-fast'; reasoningEffort='xhigh'; reasoningLabel='Extra High'
    cli=$grok; worktree=$Worktree; promptFile=$PromptFile; readOnly=[bool]$ReadOnly
    head=(& git -C $Worktree rev-parse HEAD); startedUtc=[DateTime]::UtcNow.ToString('o')
    state='RUNNING'; noRecursiveSubagents=$true; pushAllowed=$false
}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'session.json') -Encoding utf8
$arguments=@('--model','grok-4.7-build-fast','--reasoning-effort','xhigh',
    '--cwd',$Worktree,'--session-id',$SessionId,'--prompt-file',$PromptFile,
    '--output-format','streaming-json','--no-subagents','--disable-web-search',
    '--max-turns',"$MaxTurns")
if ($ReadOnly) { $arguments+=@('--permission-mode','plan') }
else { $arguments+=@('--always-approve','--no-plan') }
try {
    & $grok @arguments 1> (Join-Path $OutputDirectory 'transcript.jsonl') 2> (Join-Path $OutputDirectory 'stderr.txt')
    $record.exitCode=$LASTEXITCODE
    $record.state=if($LASTEXITCODE -eq 0){'FINISHED'}else{'FAILED'}
} catch {
    $record.state='FAILED'; $record.error=$_.Exception.Message; $record.exitCode=1
} finally {
    $record.finishedUtc=[DateTime]::UtcNow.ToString('o')
    $record.finalHead=(& git -C $Worktree rev-parse HEAD)
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'session.json') -Encoding utf8
}
if($record.exitCode -ne 0){throw "Grok agent $Role failed; preserve $OutputDirectory"}
