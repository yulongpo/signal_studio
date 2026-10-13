param(
  [string]$VisualStudioRoot = 'C:/Program Files/Microsoft Visual Studio/18/Community',
  [string]$RuntimeRoot = 'out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRepository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$taskRuntime = (Resolve-Path -LiteralPath (Join-Path $taskRepository $RuntimeRoot)).Path
$taskOutput = Join-Path $taskRepository 'out/tasks/wideband-detection/M3/local-builds/native-probe'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskVcVars = Join-Path $VisualStudioRoot 'VC/Auxiliary/Build/vcvars64.bat'
$taskSource = Join-Path $PSScriptRoot 'model_native_probe.cpp'
$taskExecutable = Join-Path $taskOutput 'ModelNativeProbe.exe'
$taskObject = Join-Path $taskOutput 'ModelNativeProbe.obj'
$taskCompilerOptions = @(
  '/nologo', '/EHsc', '/std:c++20', '/MT', '/O2', '/W4', '/external:W0',
  "/external:I`"$taskRuntime/include`"",
  "/external:I`"$taskRuntime/nvidia/cuda_runtime/include`"",
  "`"$taskSource`"", "/Fo`"$taskObject`"", "/Fe`"$taskExecutable`"",
  '/link', "/LIBPATH:`"$taskRuntime/lib`"",
  "/LIBPATH:`"$taskRuntime/nvidia/cuda_runtime/lib/x64`"", 'nvinfer.lib', 'cudart.lib'
)
$taskCommand = "call `"$taskVcVars`" >nul && cl $($taskCompilerOptions -join ' ')"
& $env:COMSPEC /d /s /c $taskCommand
if ($LASTEXITCODE -ne 0) { throw "Native model probe compilation failed: $LASTEXITCODE" }
Copy-Item -LiteralPath (Join-Path $taskRuntime 'tensorrt_libs/nvinfer_10.dll') -Destination $taskOutput
Copy-Item -LiteralPath (Join-Path $taskRuntime 'nvidia/cuda_runtime/bin/cudart64_12.dll') -Destination $taskOutput
Write-Output $taskExecutable
