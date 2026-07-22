[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

$validator = Join-Path $RepositoryRoot 'scripts\validate-dependency-lock.ps1'
$testRoot = Join-Path $RepositoryRoot ("build\dependency-mode-tests\{0}-{1}" -f $PID, [guid]::NewGuid().ToString('N'))
$resolvedRepository = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
$resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
if (-not $resolvedTestRoot.StartsWith($resolvedRepository + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Dependency mode test root escaped the repository: $resolvedTestRoot"
}
$null = [IO.Directory]::CreateDirectory($resolvedTestRoot)

function Write-TestInventory {
    param(
        [Parameter(Mandatory = $true)][object]$Document,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $path = Join-Path $resolvedTestRoot $Name
    [IO.File]::WriteAllText($path, (($Document | ConvertTo-Json -Depth 8) + "`n"), [Text.UTF8Encoding]::new($false))
    return $path
}

function Invoke-ValidationSuccess {
    param([Parameter(Mandatory = $true)][hashtable]$Arguments)
    $output = (& $validator @Arguments | Out-String)
    return $output | ConvertFrom-Json
}

function Assert-ValidationRejected {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Arguments,
        [Parameter(Mandatory = $true)][string]$ExpectedMessage
    )
    try {
        $null = & $validator @Arguments
    }
    catch {
        if (-not $_.Exception.Message.Contains($ExpectedMessage)) {
            throw "Validation failed for an unexpected reason: $($_.Exception.Message)"
        }
        return
    }
    throw "Validation unexpectedly accepted an incompatible host: $ExpectedMessage"
}

try {
    $acquisition = Invoke-ValidationSuccess -Arguments @{ Mode = 'Acquisition'; Headless = $true }
    if ($acquisition.mode -cne 'Acquisition' -or $acquisition.verified_selected_package_tuples -ne 14 -or
        $acquisition.current_host_evidence) {
        throw 'Acquisition mode did not remain independent from host inventory'
    }

    $alternate = [ordered]@{
        schema = 'signal-studio.test-host-evidence/1.0'
        platform = 'windows'
        architecture = 'x64'
        tools = @(
            [ordered]@{name='Git';tool_family='git';architecture='x64';state='detected';version='2.54.7.windows.1';path='E:/portable/git.exe';file_sha256=('1' * 64);hash_scope='simulated'},
            [ordered]@{name='CMake';tool_family='cmake';architecture='x64';state='detected';version='4.4.9';path='E:/portable/cmake.exe';file_sha256=('2' * 64);hash_scope='simulated'},
            [ordered]@{name='Ninja';tool_family='ninja';architecture='x64';state='detected';version='1.13.2';path='E:/portable/ninja.exe';file_sha256=('3' * 64);hash_scope='simulated'},
            [ordered]@{name='Qt';tool_family='qt-msvc2022';architecture='x64';state='detected';version='6.10.9';path='E:/portable/Qt/bin/qmake.exe';file_sha256=('4' * 64);hash_scope='simulated'},
            [ordered]@{name='MSVC';tool_family='msvc';architecture='x64';state='detected';version='19.45.99999';path='E:/portable/MSVC/cl.exe';file_sha256=('5' * 64);hash_scope='simulated'},
            [ordered]@{name='Windows SDK';tool_family='windows-sdk';architecture='x64';state='detected';version='10.0.26100.123';path='E:/portable/SDK/rc.exe';file_sha256=('6' * 64);hash_scope='simulated'},
            [ordered]@{name='Python';tool_family='cpython';architecture='x64';state='detected';version='3.13.13';path='E:/portable/python.exe';file_sha256=('7' * 64);hash_scope='simulated'}
        )
    }
    $alternatePath = Write-TestInventory -Document $alternate -Name 'compatible-alternate.json'
    $emittedPath = Join-Path $resolvedTestRoot 'emitted-current-host.json'
    $compatible = Invoke-ValidationSuccess -Arguments @{
        Mode = 'CompatibleHost'
        ObservedInventoryPath = $alternatePath
        HostEvidenceOutputPath = $emittedPath
    }
    if ($compatible.mode -cne 'CompatibleHost' -or $compatible.compatible_tools.Count -ne 7 -or
        -not (Test-Path -LiteralPath $emittedPath -PathType Leaf)) {
        throw 'CompatibleHost mode did not accept alternate compatible paths and patch versions'
    }
    $emitted = Get-Content -Raw -Encoding UTF8 -LiteralPath $emittedPath | ConvertFrom-Json
    if (($emitted.tools | Where-Object { $_.name -eq 'Git' }).path -cne 'E:/portable/git.exe') {
        throw 'CompatibleHost mode did not emit the observed alternate host evidence'
    }

    $incompatibleVersion = $alternate | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    ($incompatibleVersion.tools | Where-Object { $_.name -eq 'CMake' }).version = '3.27.9'
    $incompatibleVersionPath = Write-TestInventory -Document $incompatibleVersion -Name 'incompatible-version.json'
    Assert-ValidationRejected -Arguments @{
        Mode = 'CompatibleHost'; ObservedInventoryPath = $incompatibleVersionPath; HostEvidenceOutputPath = $emittedPath
    } -ExpectedMessage 'Tool version is incompatible for CMake'

    $incompatibleFamily = $alternate | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    ($incompatibleFamily.tools | Where-Object { $_.name -eq 'Ninja' }).tool_family = 'make'
    $incompatibleFamilyPath = Write-TestInventory -Document $incompatibleFamily -Name 'incompatible-family.json'
    Assert-ValidationRejected -Arguments @{
        Mode = 'CompatibleHost'; ObservedInventoryPath = $incompatibleFamilyPath; HostEvidenceOutputPath = $emittedPath
    } -ExpectedMessage 'Tool family mismatch for Ninja'

    $incompatibleArchitecture = $alternate | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $incompatibleArchitecture.architecture = 'x86'
    $incompatibleArchitecturePath = Write-TestInventory -Document $incompatibleArchitecture -Name 'incompatible-architecture.json'
    Assert-ValidationRejected -Arguments @{
        Mode = 'CompatibleHost'; ObservedInventoryPath = $incompatibleArchitecturePath; HostEvidenceOutputPath = $emittedPath
    } -ExpectedMessage 'Host platform/architecture is incompatible'

    $capturedPath = Join-Path $RepositoryRoot 'dependencies\captured-host-evidence.json'
    $exact = Invoke-ValidationSuccess -Arguments @{
        Mode = 'ExactCapturedHost'; ObservedInventoryPath = $capturedPath; HostEvidenceOutputPath = $emittedPath
    }
    if (-not $exact.exact_captured_host -or $exact.compatible_tools.Count -ne 7) {
        throw 'ExactCapturedHost mode did not accept the committed captured inventory'
    }

    $exactMismatch = Get-Content -Raw -Encoding UTF8 -LiteralPath $capturedPath | ConvertFrom-Json
    ($exactMismatch.tools | Where-Object { $_.name -eq 'Git' }).path = 'E:/different/git.exe'
    $exactMismatchPath = Write-TestInventory -Document $exactMismatch -Name 'exact-path-mismatch.json'
    Assert-ValidationRejected -Arguments @{
        Mode = 'ExactCapturedHost'; ObservedInventoryPath = $exactMismatchPath; HostEvidenceOutputPath = $emittedPath
    } -ExpectedMessage 'Exact captured-host mismatch for Git'

    Write-Host 'Verified acquisition-only, compatible alternate host, incompatible version/family/architecture, and exact captured-host modes.'
}
finally {
    if (Test-Path -LiteralPath $resolvedTestRoot -PathType Container) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
