[CmdletBinding()]
param([string]$QtRoot=$env:SS_QT_ROOT, [string]$OutputDirectory, [string]$VsRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $repo 'out/tasks/ux-r6/local-builds/gpu-dsp-probe' }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if (-not $QtRoot) {
  $cache=Join-Path $repo 'out/vs2026-qt611-release/CMakeCache.txt'
  if (Test-Path -LiteralPath $cache) {
    $line=Get-Content -LiteralPath $cache | Where-Object { $_ -like 'SS_QT_ROOT:PATH=*' } | Select-Object -First 1
    if ($line) { $QtRoot=$line.Substring($line.IndexOf('=')+1) }
  }
}
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib/Qt6Core.lib'))) { throw 'Pass -QtRoot with Qt 6.11.1 MSVC x64' }
if (-not $VsRoot) {
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  $VsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$vcvars=Join-Path $VsRoot 'VC/Auxiliary/Build/vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw 'Cannot discover VS x64 toolchain' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$compile=@('/nologo','/c','/O2','/MD','/EHsc','/std:c++20','/utf-8','/Zc:__cplusplus','/DNOMINMAX',
  "/I`"$repo`"", "/I`"$QtRoot/include`"", "/I`"$QtRoot/include/QtCore`"", "/I`"$QtRoot/mkspecs/win32-msvc`"",
  '/DQT_CORE_LIB',"`"$repo/infrastructure/gpu_dsp.cpp`"", "`"$repo/tools/gpu_dsp_probe/main.cpp`"", "/Fo`"$OutputDirectory/`"")
$compile | Set-Content (Join-Path $OutputDirectory 'compile.rsp') -Encoding utf8
$link=@('/NOLOGO','/SUBSYSTEM:CONSOLE',"/OUT:`"$OutputDirectory/SignalStudioGpuDspProbe.exe`"",
  "`"$OutputDirectory/gpu_dsp.obj`"", "`"$OutputDirectory/main.obj`"", "`"$QtRoot/lib/Qt6Core.lib`"",'userenv.lib','mpr.lib','shell32.lib','advapi32.lib')
$link | Set-Content (Join-Path $OutputDirectory 'link.rsp') -Encoding utf8
@('@echo off', "call `"$vcvars`" >nul", "cl @`"$OutputDirectory/compile.rsp`"", 'if errorlevel 1 exit /b %errorlevel%',
  "link @`"$OutputDirectory/link.rsp`"", 'exit /b %errorlevel%') | Set-Content (Join-Path $OutputDirectory 'build.cmd') -Encoding ascii
& $env:ComSpec /c (Join-Path $OutputDirectory 'build.cmd')
if ($LASTEXITCODE -ne 0) { throw "Standalone GPU probe build failed: $LASTEXITCODE" }
Copy-Item -LiteralPath (Join-Path $QtRoot 'bin/Qt6Core.dll') -Destination $OutputDirectory -Force
Copy-Item -LiteralPath (Join-Path $repo 'scripts/gpu-dsp-golden.py') -Destination $OutputDirectory -Force
Write-Output (Join-Path $OutputDirectory 'SignalStudioGpuDspProbe.exe')
