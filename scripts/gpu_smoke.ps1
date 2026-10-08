[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$SoftwareRenderer
)
$ErrorActionPreference = 'Stop'

# The application implements the GPU acceptance condition: the QRhi driver
# must be non-CPU and at least one texture upload must occur.
# SoftwareRenderer validates the explicit QWidget fallback and reports it as such.
$verifyScript = Join-Path $PSScriptRoot 'verify-package.ps1'
& $verifyScript -Configuration $Configuration -SoftwareRenderer:$SoftwareRenderer
