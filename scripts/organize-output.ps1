[CmdletBinding()]
param([switch]$Apply)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$taskOutRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out'))
$taskOutPrefix = $taskOutRoot + [IO.Path]::DirectorySeparatorChar
$taskPlan = [Collections.Generic.List[object]]::new()

function Get-BoundedOutputPath([string]$RelativePath) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $taskOutRoot $RelativePath))
    if (-not $resolved.StartsWith($taskOutPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Output path escapes the workspace output directory: $RelativePath"
    }
    $ancestor = $resolved
    while (-not $ancestor.Equals($taskOutRoot, [StringComparison]::OrdinalIgnoreCase)) {
        if ((Test-Path -LiteralPath $ancestor) -and
            ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Output path traverses a reparse point: $RelativePath"
        }
        $ancestor = Split-Path -Parent $ancestor
    }
    return $resolved
}

function Add-OutputMove([string]$Source, [string]$Destination) {
    $sourcePath = Get-BoundedOutputPath $Source
    if (-not (Test-Path -LiteralPath $sourcePath)) { return }
    $destinationPath = Get-BoundedOutputPath $Destination
    if (Test-Path -LiteralPath $destinationPath) {
        throw "Refusing to overwrite an existing output: $Destination"
    }
    $item = Get-Item -LiteralPath $sourcePath -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to move a reparse point: $Source"
    }
    $taskPlan.Add([pscustomobject]@{
        source = $Source
        destination = $Destination
        directory = $item.PSIsContainer
    })
}

function Get-OutputFingerprint([string]$Path, [bool]$Directory) {
    if (-not $Directory) {
        $file = Get-Item -LiteralPath $Path
        return [pscustomobject]@{
            files = 1
            bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $entries = @(Get-ChildItem -LiteralPath $Path -Recurse -Force)
    if (@($entries | Where-Object {
        $_.Attributes -band [IO.FileAttributes]::ReparsePoint
    }).Count -ne 0) {
        throw "Refusing to enumerate output through reparse points: $Path"
    }
    $files = @($entries | Where-Object { -not $_.PSIsContainer })
    $bytes = ($files | Measure-Object Length -Sum).Sum
    return [pscustomobject]@{
        files = $files.Count
        bytes = if ($null -eq $bytes) { 0 } else { $bytes }
        sha256 = $null
    }
}

if (-not (Test-Path -LiteralPath $taskOutRoot -PathType Container)) {
    throw 'Workspace output directory is absent.'
}
if ((Get-Item -LiteralPath $taskOutRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Workspace output directory must not be a reparse point.'
}

# Preset build trees and deployed applications stay at their canonical paths.
foreach ($item in Get-ChildItem -LiteralPath $taskOutRoot -File -Force) {
    if ($item.Name -eq 'README.md') { continue }
    $category = if ($item.Extension -in @('.json', '.jsonl')) { 'diagnostics' } else { 'logs' }
    if ($item.Name -match '^m([0-5])-') {
        $destination = "tasks/wideband-detection/M$($Matches[1])/$category/$($item.Name)"
    } else {
        $history = switch -Regex ($item.Name) {
            '^parameter-input-' { 'parameter-input'; break }
            '^display-|^screen-inventory' { 'display'; break }
            '^chart-' { 'chart-regression'; break }
            '^spectral-' { 'spectral-analysis'; break }
            '^ui-parity-' { 'ui-parity'; break }
            'linked|^focused-' { 'linked-analysis'; break }
            default { 'general' }
        }
        $destination = "tasks/history/$history/$category/$($item.Name)"
    }
    Add-OutputMove $item.Name $destination
}

Add-OutputMove 'annotation-core-local' 'tasks/wideband-detection/M1/local-builds/annotation-core'
Add-OutputMove 'parameter-input-native' 'tasks/history/parameter-input/native'
Add-OutputMove 'wideband-tools' 'toolchains/wideband-python'
foreach ($name in @('build', 'chart-fixes-release-verify', 'chart-fixes-release-verify-bin',
                   'nb-color-fix-verify-release_bin', 'nb-v143')) {
    Add-OutputMove $name "archive/builds/$name"
}
foreach ($name in @('test-temp', 'test-tmp')) {
    Add-OutputMove $name "archive/temp/20261013/$name"
}
foreach ($node in @('M2', 'M3', 'M4')) {
    Add-OutputMove "deferred-wideband/20261013/$node" "tasks/wideband-detection/$node/deferred/20261013"
}
Add-OutputMove 'deferred-wideband/20261013/README.txt' 'tasks/output-organization/20261013/deferred-note.txt'
foreach ($name in @('controls', 'epoch1200', 'runtime')) {
    Add-OutputMove "models/$name" "tasks/wideband-detection/M3/assets/$name"
}
Add-OutputMove 'models/native-probe' 'tasks/wideband-detection/M3/local-builds/native-probe'
if (Test-Path -LiteralPath (Join-Path $taskOutRoot 'models')) {
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $taskOutRoot 'models') -File -Force) {
        Add-OutputMove "models/$($item.Name)" "tasks/wideband-detection/M3/logs/$($item.Name)"
    }
}

$indexRoot = Get-BoundedOutputPath 'tasks/output-organization/20261013'
New-Item -ItemType Directory -Path $indexRoot -Force | Out-Null
$planPath = Join-Path $indexRoot 'plan.json'
if (Test-Path -LiteralPath $planPath) {
    $planPath = Join-Path $indexRoot ("plan-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff') + '.json')
}
$taskPlan | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $planPath -Encoding utf8
if (-not $Apply) {
    Write-Output "Output relocation plan: $($taskPlan.Count) items. Use -Apply to execute."
    exit 0
}

$completed = [Collections.Generic.List[object]]::new()
$ledgerPath = Join-Path $indexRoot 'relocation.json'
if (Test-Path -LiteralPath $ledgerPath) {
    foreach ($previous in @(Get-Content -LiteralPath $ledgerPath -Raw | ConvertFrom-Json)) {
        $completed.Add($previous)
    }
}
foreach ($move in $taskPlan) {
    $sourcePath = Get-BoundedOutputPath $move.source
    $destinationPath = Get-BoundedOutputPath $move.destination
    $before = Get-OutputFingerprint $sourcePath $move.directory
    New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
    Move-Item -LiteralPath $sourcePath -Destination $destinationPath
    $after = Get-OutputFingerprint $destinationPath $move.directory
    if ($before.files -ne $after.files -or $before.bytes -ne $after.bytes -or
        $before.sha256 -ne $after.sha256) {
        throw "Relocated output failed identity validation: $($move.source)"
    }
    $completed.Add([pscustomobject]@{
        source = $move.source
        destination = $move.destination
        directory = $move.directory
        files = $after.files
        bytes = $after.bytes
        sha256 = $after.sha256
        verified = $true
    })
    # Persist progress after every verified move, so partial failure is reviewable.
    $completed | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ledgerPath -Encoding utf8
}

foreach ($relative in @('models', 'deferred-wideband/20261013', 'deferred-wideband')) {
    $emptyPath = Get-BoundedOutputPath $relative
    if ((Test-Path -LiteralPath $emptyPath -PathType Container) -and
        @(Get-ChildItem -LiteralPath $emptyPath -Force).Count -eq 0) {
        Remove-Item -LiteralPath $emptyPath
    }
}
Write-Output "Output relocation complete: $($completed.Count) verified items."
