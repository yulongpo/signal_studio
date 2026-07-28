[CmdletBinding()]
param(
    [ValidateSet('intel-mkl', 'libsamplerate', 'benchmark')]
    [string[]]$Component = @('intel-mkl', 'libsamplerate', 'benchmark'),
    [switch]$DownloadOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$dependencyRoot = Join-Path $repositoryRoot '.deps'
$vcpkgCommit = '82b6bc886d7b0f8342e34babc2e0b8943f79b0e1'
$vcpkgArchiveUrl = "https://github.com/microsoft/vcpkg/archive/$vcpkgCommit.tar.gz"
$vcpkgArchiveSha256 = '550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a'
$cacheRoot = Join-Path $dependencyRoot 'cache'
$downloadsRoot = Join-Path $dependencyRoot 'downloads'
$binaryCacheRoot = Join-Path $dependencyRoot 'binary-cache'
$archive = Join-Path $cacheRoot "vcpkg-$vcpkgCommit.tar.gz"
$vcpkgRoot = Join-Path $dependencyRoot "vcpkg-$vcpkgCommit"
$vcpkg = Join-Path $vcpkgRoot 'vcpkg.exe'
$installRoot = Join-Path $dependencyRoot 'vcpkg_installed'

New-Item -ItemType Directory -Force $cacheRoot, $downloadsRoot, $binaryCacheRoot | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -UseBasicParsing $vcpkgArchiveUrl -OutFile $archive
}
$actualArchiveSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualArchiveSha256 -ne $vcpkgArchiveSha256) {
    throw "vcpkg 归档校验失败：期望 $vcpkgArchiveSha256，实际 $actualArchiveSha256"
}

if (-not (Test-Path -LiteralPath $vcpkg)) {
    New-Item -ItemType Directory -Force $dependencyRoot | Out-Null
    tar -xf $archive -C $dependencyRoot
    & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        throw 'vcpkg bootstrap 失败。'
    }
}

$ports = foreach ($name in $Component) {
    switch ($name) {
        'intel-mkl' { 'intel-mkl:x64-windows' }
        'libsamplerate' { 'libsamplerate:x64-windows' }
        'benchmark' { 'benchmark:x64-windows' }
    }
}
$arguments = @(
    'install'
    '--classic'
    "--x-install-root=$installRoot"
    "--downloads-root=$downloadsRoot"
    '--disable-metrics'
)
if ($DownloadOnly) {
    $arguments += '--only-downloads'
}
$arguments += $ports

$previousManifestMode = $env:VCPKG_MANIFEST_MODE
$previousBinarySources = $env:VCPKG_BINARY_SOURCES
$env:VCPKG_MANIFEST_MODE = 'OFF'
$env:VCPKG_BINARY_SOURCES = "clear;files,$binaryCacheRoot,readwrite"
try {
    & $vcpkg @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "MS-02 最小依赖安装失败，退出码 $LASTEXITCODE。"
    }
}
finally {
    $env:VCPKG_MANIFEST_MODE = $previousManifestMode
    $env:VCPKG_BINARY_SOURCES = $previousBinarySources
}

[ordered]@{
    schema = 'signal-studio.ms02-dependencies/1.0'
    vcpkg_commit = $vcpkgCommit
    install_root = $installRoot
    binary_cache_root = $binaryCacheRoot
    installed_components = $Component
    cuda_policy = '本机 CUDA 12.4.131 的 cudart/cuFFT 作为可选后端；本脚本不安装 CUDA 或 cuDNN。'
} | ConvertTo-Json -Depth 4
