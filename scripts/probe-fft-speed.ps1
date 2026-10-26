[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $repo 'out/tasks/ux-r6/diagnostics/fft-speed-probe'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$build = Join-Path $repo 'out/tasks/ux-r6/local-builds/fft-speed-probe'
New-Item -ItemType Directory -Force $OutputDirectory, $build | Out-Null
$prefix = Join-Path $repo 'out/toolchains/fft-libs/conda/Library'
$nvcc = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.4\bin\nvcc.exe'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $repo 'tools/fft_speed_probe/main.cu'
$exe = Join-Path $build 'fft_speed_probe.exe'
$cmd = Join-Path $build 'build.cmd'
@"
@echo off
call "$vcvars" >nul
"$nvcc" -allow-unsupported-compiler -O3 -std=c++17 --expt-relaxed-constexpr -gencode=arch=compute_89,code=compute_89 "$source" -o "$exe" -lcufft
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $cmd -Encoding ascii
& $env:ComSpec /c $cmd
if ($LASTEXITCODE -ne 0) { throw "FFT speed probe build failed: $LASTEXITCODE" }
$env:SS_FFTW_DLL = Join-Path $prefix 'bin/fftw3.dll'
$env:SS_MKL_DLL = Join-Path $prefix 'bin/mkl_rt.3.dll'
$env:PATH = "$(Join-Path $prefix 'bin');$env:PATH"
& $exe *>&1 | Tee-Object (Join-Path $OutputDirectory 'stdout.jsonl')
$result = $LASTEXITCODE
"EXIT_CODE=$result" | Set-Content (Join-Path $OutputDirectory 'exit-code.txt')
if ($result -ne 0) { throw "FFT speed probe failed: $result" }
Write-Output $OutputDirectory
