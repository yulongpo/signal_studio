[CmdletBinding(SupportsShouldProcess)]
param([switch]$Unregister,[string]$Executable)
$ErrorActionPreference = 'Stop'
$programId = 'SignalStudio.Project.1'
$classes = 'HKCU:\Software\Classes'
$extension = Join-Path $classes '.ssproj'
$program = Join-Path $classes $programId
if ($Unregister) {
    if ($PSCmdlet.ShouldProcess('Current user .ssproj association', 'Unregister')) {
        if (Test-Path -LiteralPath $extension) {
            if ((Get-Item -LiteralPath $extension).GetValue('') -eq $programId) {
                $writableKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes\.ssproj', $true)
                try { $writableKey.DeleteValue('', $false) } finally { $writableKey.Dispose() }
            }
            Remove-ItemProperty -LiteralPath (Join-Path $extension 'OpenWithProgids') -Name $programId -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $program) { Remove-Item -LiteralPath $program -Recurse }
    }
    return
}
if (-not $Executable) { $Executable = Join-Path (Split-Path -Parent $PSScriptRoot) 'out/vs2026-qt611-release_bin/SignalStudio.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if ([IO.Path]::GetFileName($Executable) -ne 'SignalStudio.exe') { throw 'Select the deployed SignalStudio.exe' }
if ($PSCmdlet.ShouldProcess("Current user .ssproj -> $Executable", 'Register')) {
    New-Item -Path $extension -Force | Out-Null
    New-Item -Path (Join-Path $extension 'OpenWithProgids') -Force | Out-Null
    New-ItemProperty -LiteralPath (Join-Path $extension 'OpenWithProgids') -Name $programId -Value '' -PropertyType String -Force | Out-Null
    # Respect a pre-existing default; registration remains available in Open With.
    if (-not (Get-Item -LiteralPath $extension).GetValue('')) { Set-Item -LiteralPath $extension -Value $programId }
    New-Item -Path (Join-Path $program 'shell/open/command') -Force | Out-Null
    Set-Item -LiteralPath $program -Value 'Signal Studio Project'
    Set-Item -LiteralPath (Join-Path $program 'shell/open/command') -Value ('"{0}" --project "%1"' -f $Executable)
}
