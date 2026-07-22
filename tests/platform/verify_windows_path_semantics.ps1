[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

. (Join-Path $RepositoryRoot 'scripts\common.ps1')

function Assert-CanonicalPath {
    param(
        [Parameter(Mandatory = $true)][string]$InputPath,
        [Parameter(Mandatory = $true)][string]$Expected
    )
    $actual = Get-SignalStudioCanonicalPathEntry -PathEntry $InputPath
    if (-not $actual.Equals($Expected, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Canonical path mismatch: '$InputPath' -> '$actual', expected '$Expected'"
    }
}

Assert-CanonicalPath -InputPath 'C:\' -Expected 'C:\'
Assert-CanonicalPath -InputPath 'C:\\' -Expected 'C:\'
Assert-CanonicalPath -InputPath 'C:\Temp\' -Expected 'C:\Temp'
Assert-CanonicalPath -InputPath 'C:\Temp\\' -Expected 'C:\Temp'
Assert-CanonicalPath -InputPath '\\server\share' -Expected '\\server\share\'
Assert-CanonicalPath -InputPath '\\server\share\' -Expected '\\server\share\'
Assert-CanonicalPath -InputPath '\\server\share\folder\' -Expected '\\server\share\folder'
Assert-CanonicalPath -InputPath '\\?\C:\' -Expected '\\?\C:\'
Assert-CanonicalPath -InputPath '\\?\C:\Temp\' -Expected '\\?\C:\Temp'
Assert-CanonicalPath -InputPath '\\?\UNC\server\share\' -Expected '\\?\UNC\server\share\'
Assert-CanonicalPath -InputPath '\\?\UNC\server\share\folder\' -Expected '\\?\UNC\server\share\folder'

$normalized = Get-SignalStudioNormalizedPath -PathValue (
    'C:\;c:\;C:\Temp\;c:\temp\\;\\server\share\;\\SERVER\SHARE;' +
    '\\server\share\folder\;\\SERVER\SHARE\FOLDER'
)
$entries = @($normalized -split ';' | Where-Object { $_ })
if ($entries.Count -ne 4) { throw "Expected four unique path entries, observed $($entries.Count): $normalized" }
if ($entries[0] -cne 'C:\' -or $entries[1] -cne 'C:\Temp' -or
    $entries[2] -cne '\\server\share\' -or $entries[3] -cne '\\server\share\folder') {
    throw "Canonical path order or root semantics changed: $normalized"
}

$withoutDriveRoot = Get-SignalStudioNormalizedPath -PathValue $normalized -Exclude @('c:\')
if (($withoutDriveRoot -split ';') -contains 'C:\') { throw 'Drive root exclusion did not use canonical semantics' }

Write-Host "Verified Windows drive, UNC, extended-root, duplicate, case, and trailing-separator semantics: $normalized"
