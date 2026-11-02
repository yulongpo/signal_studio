[CmdletBinding()]
param([string]$Nvcc='nvcc',[string]$Output,[string]$HostVcVars)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output=Join-Path $repo 'out/tasks/ux-r6/assets/gpu-dsp/SignalStudioGpuDspCuda.dll' }
$Output=[IO.Path]::GetFullPath($Output)
if ([IO.Path]::GetExtension($Output) -ne '.dll') { throw 'CUB requires host CUDA: -Output must name a DLL, not PTX' }
$nvccPath=(Get-Command $Nvcc -ErrorAction Stop).Source
$cudaRoot=Split-Path -Parent (Split-Path -Parent $nvccPath)
if (-not $HostVcVars) {
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  $root=& $vswhere -latest -version '[16.0,17.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($root) { $HostVcVars=Join-Path $root 'VC/Auxiliary/Build/vcvars64.bat' }
}
if (-not $HostVcVars -or -not (Test-Path -LiteralPath $HostVcVars)) { throw 'Pass a compatible installed VS2019 x64 -HostVcVars' }
$directory=Split-Path -Parent $Output
New-Item -ItemType Directory -Force $directory | Out-Null
$args=@('--shared','--cudart=static','--std=c++17','-O3','--fmad=false','--ftz=false','--prec-div=true','--prec-sqrt=true',
  '-gencode=arch=compute_89,code=[sm_89,compute_89]', '-Xcompiler=/MT', '-Xcompiler=/EHsc', '-Xcompiler=/utf-8',
  "-I`"$repo`"", "`"$repo/infrastructure/gpu_dsp_kernels.cu`"", "-L`"$cudaRoot/lib/x64`"",'-lcufft','-lcuda','-o',"`"$Output`"")
# No unsupported-compiler override: the chosen toolchain must actually compile.
$args | Set-Content (Join-Path $directory 'module-build.rsp') -Encoding ascii
@('@echo off',"call `"$HostVcVars`" >nul", "cd /d `"$directory`"", "`"$nvccPath`" --options-file `"$directory/module-build.rsp`"",'exit /b %errorlevel%') |
  Set-Content (Join-Path $directory 'module-build.cmd') -Encoding ascii
Write-Host 'Building CUDA17 pure-POD ABI DLL with cuFFT/CUB, static CUDA runtime, embedded compute_89 PTX; no fastmath'
& $env:ComSpec /c (Join-Path $directory 'module-build.cmd')
if ($LASTEXITCODE -ne 0) { throw "CUDA module build failed: $LASTEXITCODE" }
Write-Output $Output
