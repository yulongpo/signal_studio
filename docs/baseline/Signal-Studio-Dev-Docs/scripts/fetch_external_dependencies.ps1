param([string]$Destination = "$PSScriptRoot\offline-cache")
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Destination | Out-Null
$url = "https://developer.download.nvidia.com/compute/cuda/12.8.1/network_installers/cuda_12.8.1_windows_network.exe"
$out = Join-Path $Destination "cuda_12.8.1_windows_network.exe"
Invoke-WebRequest -UseBasicParsing $url -OutFile $out
$expected = "779bee8ff557255c1cf5f36e0230f081675b9bb41e44be38839920cd5209bdeb"
if ((Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw "CUDA installer checksum mismatch" }
Write-Host "Verified. Installation is manual and requires acceptance of the NVIDIA CUDA EULA."
