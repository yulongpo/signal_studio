[CmdletBinding()]
param(
    [ValidateSet('local-windows-msvc-cpu-release', 'local-windows-msvc-cuda-release')]
    [string]$Preset = 'local-windows-msvc-cpu-release'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = [IO.Path]::GetFullPath($script:RepositoryRoot)
$buildDirectory = Join-Path $repositoryRoot "build\$Preset"
$binaryDirectory = Join-Path $buildDirectory 'bin'
$stagingRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "build\runtime-closure\$Preset"))
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build\runtime-closure'))
if (-not $stagingRoot.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "运行时闭包暂存目录越界：$stagingRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $binaryDirectory 'signal_studio_dsp_tests.exe'))) {
    throw "缺少 MS-02 测试程序，请先构建 $Preset。"
}

$cmake = Get-SignalStudioCMake
& $cmake -E remove_directory $stagingRoot
if ($LASTEXITCODE -ne 0) {
    throw "无法清理旧运行时闭包暂存目录。"
}
New-Item -ItemType Directory -Force $stagingRoot | Out-Null

$requiredFiles = @(
    'signal_studio_dsp_tests.exe',
    'SignalStudioCompute.dll',
    'SignalStudioDSP.dll',
    'concrt140.dll',
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll',
    'vccorlib140.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll',
    'vcruntime140_threads.dll',
    'mkl_core.2.dll',
    'mkl_sequential.2.dll',
    'mkl_def.2.dll',
    'mkl_avx2.2.dll',
    'mkl_avx512.2.dll',
    'mkl_mc3.2.dll',
    'mkl_vml_def.2.dll',
    'mkl_vml_avx2.2.dll',
    'mkl_vml_avx512.2.dll',
    'mkl_vml_mc3.2.dll',
    'mkl_vml_cmpt.2.dll',
    'samplerate.dll'
)
$optionalCudaFiles = @('cudart64_12.dll', 'cufft64_11.dll')
$cudaRequired = $Preset -like '*-cuda-*'
foreach ($name in $requiredFiles) {
    $source = Join-Path $binaryDirectory $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "精确运行时闭包缺少：$name"
    }
    Copy-Item -LiteralPath $source -Destination $stagingRoot
}
foreach ($name in $optionalCudaFiles) {
    $source = Join-Path $binaryDirectory $name
    if ($cudaRequired -and -not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "CUDA 运行时闭包缺少：$name"
    }
    if ($cudaRequired) {
        Copy-Item -LiteralPath $source -Destination $stagingRoot
    }
}
if (-not $cudaRequired) {
    foreach ($name in $optionalCudaFiles) {
        if (Test-Path -LiteralPath (Join-Path $stagingRoot $name) -PathType Leaf) {
            throw "纯 CPU 运行时闭包不得包含 CUDA 组件：$name"
        }
    }
}

$forbiddenPatterns = @('mkl_sycl*.dll', 'mkl_blacs*.dll', 'mkl_scalapack*.dll',
    'mkl_*thread*.dll', 'libiomp*.dll', 'tbb*.dll')
foreach ($pattern in $forbiddenPatterns) {
    if (Get-ChildItem -LiteralPath $stagingRoot -Filter $pattern -File) {
        throw "运行时闭包包含禁止组件：$pattern"
    }
}

$previousPath = $env:PATH
$previousCudaPath = $env:CUDA_PATH
try {
    $env:PATH = "$stagingRoot;$env:SystemRoot\System32;$env:SystemRoot"
    $env:CUDA_PATH = $null
    & (Join-Path $stagingRoot 'signal_studio_dsp_tests.exe') --case FR-DSP-101
    if ($LASTEXITCODE -ne 0) {
        throw "洁净 PATH 下 MS-02 DSP/CPU/CUDA 运行时闭包验证失败，退出码：$LASTEXITCODE"
    }
}
finally {
    $env:PATH = $previousPath
    $env:CUDA_PATH = $previousCudaPath
}

[ordered]@{
    schema = 'signal-studio.ms02-runtime-closure/1.0'
    preset = $Preset
    cuda_required = $cudaRequired
    staging_root = $stagingRoot
    files = @(Get-ChildItem -LiteralPath $stagingRoot -File | Sort-Object Name | ForEach-Object Name)
    clean_path_execution = 'FR-DSP-101 PASS'
} | ConvertTo-Json -Depth 4
