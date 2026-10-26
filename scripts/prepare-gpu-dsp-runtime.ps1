[CmdletBinding()]
param([switch]$Download, [string]$OutputDirectory, [string]$LocalCudaRoot, [string]$ModuleLibrary,
      [string]$CufftVersion='11.4.1.4', [string]$NvJitLinkVersion='12.9.86')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $repo 'out/toolchains/gpu-dsp-cu129' }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$allowed=@([IO.Path]::GetFullPath((Join-Path $repo 'out/toolchains')),[IO.Path]::GetFullPath((Join-Path $repo 'out/tasks/ux-r6/assets/runtime')))
if (-not ($allowed | Where-Object { $OutputDirectory.StartsWith($_+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or $OutputDirectory -eq $_ })) {
  throw 'Runtime must remain in out/toolchains or out/tasks/ux-r6/assets/runtime'
}
if ($LocalCudaRoot) {
  if (-not $ModuleLibrary) { $ModuleLibrary=Join-Path $repo 'out/tasks/ux-r6/assets/gpu-dsp/SignalStudioGpuDspCuda.dll' }
  $sources=@($ModuleLibrary,(Join-Path $LocalCudaRoot 'bin/cufft64_11.dll'),(Join-Path $LocalCudaRoot 'bin/nvJitLink_120_0.dll'))
  foreach ($file in $sources) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing SDK/module runtime: $file" } }
  New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
  $manifest=@()
  foreach ($file in $sources) {
    $item=Get-Item -LiteralPath $file
    $target=Join-Path $OutputDirectory $item.Name
    if ($item.FullName -ne [IO.Path]::GetFullPath($target)) { Copy-Item -LiteralPath $item.FullName -Destination $target -Force }
    $manifest+=@{file=$item.Name;source=$item.FullName;bytes=$item.Length;fileVersion=$item.VersionInfo.FileVersion}
  }
  $manifest | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutputDirectory 'local-sdk-runtime.json') -Encoding utf8
  Write-Output $OutputDirectory
  return
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($package in @(@('nvidia-cufft-cu12',$CufftVersion),@('nvidia-nvjitlink-cu12',$NvJitLinkVersion))) {
  $source="https://pypi.org/pypi/$($package[0])/$($package[1])/json"
  $metadata=Invoke-RestMethod -Uri $source
  $wheel=@($metadata.urls | Where-Object { $_.filename.EndsWith('win_amd64.whl') -and -not $_.yanked })
  if ($wheel.Count -ne 1) { throw "Expected exactly one official Windows wheel: $source" }
  $wheel=$wheel[0]
  if (([Uri]$wheel.url).Host -ne 'files.pythonhosted.org') { throw 'Unexpected download host' }
  Write-Host "$($package[0])==$($package[1]) $($wheel.size) bytes; $source"
  if (-not $Download) { continue }
  New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
  $archive=Join-Path $OutputDirectory $wheel.filename
  if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $wheel.url -OutFile $archive }
  if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $wheel.digests.sha256) { throw "Wheel integrity mismatch: $archive" }
  $zip=[IO.Compression.ZipFile]::OpenRead($archive)
  try {
    foreach ($entry in $zip.Entries) {
      # Only required runtime DLLs and their license/metadata; no global pip install.
      if ($entry.FullName -notmatch '\.dll$|(?i:license|METADATA)$') { continue }
      $target=[IO.Path]::GetFullPath((Join-Path $OutputDirectory $entry.FullName))
      if (-not $target.StartsWith($OutputDirectory+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe wheel path' }
      New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
      [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$true)
      Write-Output $target
    }
  } finally { $zip.Dispose() }
}
