[CmdletBinding()]
param([Parameter(Mandatory)][string]$CufftLibrary,
      [string]$NvJitLinkLibrary, [string]$Module, [string]$Python='python', [string]$Probe, [string]$OutputDirectory,
      [switch]$Precision, [uint32]$TransformBins=0)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (-not $Probe) { $Probe=Join-Path $repo 'out/tasks/ux-r6/local-builds/gpu-dsp-probe/SignalStudioGpuDspProbe.exe' }
if (-not $Module) { $Module=Join-Path $repo 'out/tasks/ux-r6/assets/gpu-dsp/SignalStudioGpuDspCuda.dll' }
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $repo ('out/tasks/ux-r6/diagnostics/gpu-dsp/probe-'+(Get-Date -Format yyyyMMdd-HHmmss)) }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$arguments=@('--module',[IO.Path]::GetFullPath($Module),'--python',$Python,'--cufft',[IO.Path]::GetFullPath($CufftLibrary),'--output',"$OutputDirectory/measurement.json")
if ($NvJitLinkLibrary) { $arguments+=@('--nvjitlink',[IO.Path]::GetFullPath($NvJitLinkLibrary)) }
if ($Precision) { $arguments+='--precision' }
if ($TransformBins) { $arguments+=@('--transform-bins',"$TransformBins") }
& $Probe @arguments *>&1 | Tee-Object "$OutputDirectory/stdout.txt"
$result=$LASTEXITCODE
"EXIT_CODE=$result" | Set-Content "$OutputDirectory/exit-code.txt"
if ($result -ne 0) { throw "GPU DSP probe failed: $result; evidence $OutputDirectory" }
