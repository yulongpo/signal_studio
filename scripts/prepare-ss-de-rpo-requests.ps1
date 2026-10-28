[CmdletBinding()]
param(
  [string]$Manifest,
  [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Manifest) {
  $Manifest = Join-Path $repo 'docs/implementation/ux-r6/evidence/baseline/run-0c82d126-38e7-4b14-8778-491fc5c670f3-original-manifest.json'
}
$source = Get-Content -LiteralPath $Manifest -Raw -Encoding utf8 | ConvertFrom-Json
$original = if ($source.run) { $source.run.request } elseif ($source.request) { $source.request } else { $source }
if (-not (Test-Path -LiteralPath $original.sourceIdentity.path -PathType Leaf)) { throw 'Original IQ source is unavailable' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) { throw 'Use a new/empty request directory' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($count in @(20,159,363,2044)) {
  $request = $original | ConvertTo-Json -Depth 30 | ConvertFrom-Json
  $request.profile.fftSize = 65536
  $request.profile.modelFrames = 10
  $request.profile.modelHopFrames = 5
  $request.profile.hopSeconds = if ($count -eq 2044) { 0.00005 } else { 0.00025 }
  $request.parameters.timeStrategy = 'sample'
  $request.requestedRange.startSample = '0'
  $hop = [Math]::Floor($request.sourceIdentity.sampleRateHz * $request.profile.hopSeconds)
  $end = [int64](($count * 5 + 5) * $hop)
  if ($end -gt [int64]$request.sourceIdentity.availableSamples) { throw "The real source prefix is too short for $count windows" }
  $request.requestedRange.endSample = $end.ToString([Globalization.CultureInfo]::InvariantCulture)
  $request | Add-Member -NotePropertyName retainRawModelOutput -NotePropertyValue $false -Force
  $request | Add-Member -NotePropertyName rawHeadBudgetBytes -NotePropertyValue '0' -Force
  $request | Add-Member -NotePropertyName workingSetBudgetBytes -NotePropertyValue '8589934592' -Force
  $request.PSObject.Properties.Remove('legacyCombinedEvidence')
  $request | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $output "iq-$count-raw-off.json") -Encoding utf8
  if ($count -eq 20) {
    $request.retainRawModelOutput = $true
    $request.rawHeadBudgetBytes = '268435456'
    $request | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $output 'iq-20-raw-on.json') -Encoding utf8
  }
}
Write-Host 'Requests prepared from original IQ metadata; the benchmark must independently confirm every eligible/completed window count.'
