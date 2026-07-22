param([string]$Destination = "$PSScriptRoot\cache")
$ErrorActionPreference = "Stop"
$commit = "82b6bc886d7b0f8342e34babc2e0b8943f79b0e1"
$url = "https://github.com/microsoft/vcpkg/archive/$commit.tar.gz"
$sha = "550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a"
New-Item -ItemType Directory -Force $Destination | Out-Null
$archive = Join-Path $Destination "vcpkg-$commit.tar.gz"
Invoke-WebRequest -UseBasicParsing $url -OutFile $archive
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha) { throw "vcpkg archive checksum mismatch" }
Write-Host "Verified $archive"
