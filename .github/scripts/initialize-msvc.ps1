[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

if (-not $env:GITHUB_ENV) {
    throw 'GITHUB_ENV is required; this script is intended for a GitHub Actions step.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found at $vswhere"
}

$installationPath = (& $vswhere -latest -version '[17.0,18.0)' -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if (-not $installationPath) {
    throw 'A Visual Studio installation with the MSVC x64 tools was not found.'
}

$vsDevCmd = Join-Path $installationPath 'Common7/Tools/VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
    throw "VsDevCmd.bat was not found at $vsDevCmd"
}

$command = '"{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $vsDevCmd
$environmentLines = & $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "VsDevCmd.bat failed with exit code $LASTEXITCODE"
}

$protectedPrefixes = @('ACTIONS_', 'GITHUB_', 'RUNNER_')
foreach ($line in $environmentLines) {
    if (-not $line -or $line[0] -eq '=') { continue }
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    if ($protectedPrefixes | Where-Object { $name.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }) {
        continue
    }
    [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Value "$name=$value"
}

if ($env:VSCMD_ARG_TGT_ARCH -cne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -cne 'x64') {
    throw 'VsDevCmd.bat did not produce an x64-hosted x64 environment.'
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe is unavailable after importing the MSVC environment.'
}

Write-Host "Initialized MSVC x64 from $installationPath"
