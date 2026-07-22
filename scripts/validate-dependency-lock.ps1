[CmdletBinding()]
param(
    [ValidateSet('Acquisition', 'CompatibleHost', 'ExactCapturedHost')]
    [string]$Mode = 'CompatibleHost',
    [switch]$Headless,
    [string]$ObservedInventoryPath = '',
    [string]$HostEvidenceOutputPath = ''
)

. (Join-Path $PSScriptRoot 'common.ps1')

function Read-SignalStudioJson {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required JSON file is missing: $Path" }
    return Get-Content -Raw -Encoding UTF8 -LiteralPath $Path | ConvertFrom-Json
}

function ConvertTo-SignalStudioVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$ToolName
    )
    $match = [regex]::Match($Value, '[0-9]+(?:\.[0-9]+){1,3}')
    if (-not $match.Success) { throw "Tool $ToolName has an unparsable version: $Value" }
    return [version]$match.Value
}

function New-SignalStudioObservedTool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ToolFamily,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    return [ordered]@{
        name = $Name
        tool_family = $ToolFamily
        architecture = 'x64'
        state = 'detected'
        version = $Version
        path = $resolvedPath
        file_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedPath).Hash.ToLowerInvariant()
        hash_scope = 'installed executable bytes'
    }
}

$lockPath = Join-Path $script:RepositoryRoot 'dependencies\dependency-lock.json'
$cacheManifestPath = Join-Path $script:RepositoryRoot 'dependencies\offline-cache-manifest.json'
$vcpkgManifestPath = Join-Path $script:RepositoryRoot 'vcpkg.json'
$lock = Read-SignalStudioJson -Path $lockPath
$cacheManifest = Read-SignalStudioJson -Path $cacheManifestPath
$vcpkg = Read-SignalStudioJson -Path $vcpkgManifestPath
$approvedPath = Join-Path $script:RepositoryRoot $lock.approved_source
$approvedFetchPath = Join-Path $script:RepositoryRoot $lock.approved_fetch_script
$capturedHostPath = Join-Path $script:RepositoryRoot $lock.captured_host_evidence
$approved = Read-SignalStudioJson -Path $approvedPath
if (-not (Test-Path -LiteralPath $approvedFetchPath -PathType Leaf)) {
    throw "Immutable BL1.0 fetch script missing: $approvedFetchPath"
}
$approvedFetch = Get-Content -Raw -Encoding UTF8 -LiteralPath $approvedFetchPath

function Assert-SignalStudioAcquisitionContract {
    if ($lock.schema -cne 'signal-studio.dependency-lock/1.3') {
        throw "Unsupported dependency lock schema: $($lock.schema)"
    }
    $fetchAssignments = @{}
    foreach ($match in [regex]::Matches($approvedFetch, '(?m)^\$(commit|url|sha)\s*=\s*"([^"]+)"\s*$')) {
        $fetchAssignments[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    if ($fetchAssignments.Count -ne 3) { throw 'Immutable BL1.0 fetch script assignments are incomplete' }
    $approvedCommit = $fetchAssignments['commit']
    $approvedUrl = $fetchAssignments['url'].Replace('$commit', $approvedCommit)
    $approvedSha = $fetchAssignments['sha']
    $archiveMatch = [regex]::Match(
        $approvedFetch, '(?m)^\$archive\s*=\s*Join-Path\s+\$Destination\s+"([^"]+)"\s*$')
    if (-not $archiveMatch.Success) { throw 'Immutable BL1.0 fetch script archive path is missing' }
    $approvedRelativePath = $archiveMatch.Groups[1].Value.Replace('$commit', $approvedCommit)

    $qtContract = $lock.qt_compatibility_contract
    if ($qtContract.minimum_supported_version -cne '6.10.3' -or
        $qtContract.ci_validation_version -cne '6.10.3' -or
        $qtContract.local_validation_version -cne '6.11.1' -or
        $qtContract.bl1_0_qtbase_selected_version -cne '6.11.1#1' -or
        $qtContract.bl1_0_qttools_selected_version -cne '6.11.1' -or
        -not $qtContract.policy) {
        throw 'Qt compatibility contract is incomplete or inconsistent'
    }
    if ($lock.host_compatibility_contract.schema -cne 'signal-studio.host-compatibility/1.0' -or
        $lock.host_compatibility_contract.platform -cne 'windows' -or
        $lock.host_compatibility_contract.architecture -cne 'x64' -or
        @($lock.host_compatibility_contract.tools).Count -ne 7) {
        throw 'Portable host compatibility contract is incomplete'
    }
    $compatibilityNames = @($lock.host_compatibility_contract.tools | ForEach-Object { $_.name })
    if (@($compatibilityNames | Select-Object -Unique).Count -ne $compatibilityNames.Count) {
        throw 'Portable host compatibility contract contains duplicate tool names'
    }
    foreach ($tool in $lock.host_compatibility_contract.tools) {
        if (-not $tool.tool_family -or -not $tool.minimum_version -or -not $tool.maximum_version_exclusive -or
            -not ($tool.PSObject.Properties.Name -contains 'required_headless') -or
            -not ($tool.PSObject.Properties.Name -contains 'required_ui')) {
            throw "Portable compatibility contract is incomplete for $($tool.name)"
        }
        if ((ConvertTo-SignalStudioVersion -Value $tool.minimum_version -ToolName $tool.name) -ge
            (ConvertTo-SignalStudioVersion -Value $tool.maximum_version_exclusive -ToolName $tool.name)) {
            throw "Portable compatibility version range is invalid for $($tool.name)"
        }
    }

    if ($vcpkg.'builtin-baseline' -cne $lock.vcpkg.builtin_baseline -or
        $lock.vcpkg.builtin_baseline -cne $approved.vcpkg_baseline) {
        throw 'vcpkg baseline differs from immutable BL1.0'
    }
    if ($approvedCommit -cne $approved.vcpkg_baseline -or $approvedSha -cne $approved.vcpkg_archive_sha256) {
        throw 'Immutable BL1.0 fetch script differs from its dependency lock'
    }
    if ($lock.vcpkg.archive_url -cne $approvedUrl -or
        $lock.vcpkg.archive_sha256 -cne $approved.vcpkg_archive_sha256 -or
        [int64]$lock.vcpkg.archive_bytes -ne [int64]$approved.vcpkg_archive_bytes -or
        $lock.vcpkg.source_policy -cne 'exact-immutable-bl1.0-fetch-script') {
        throw 'vcpkg URL/hash/size policy differs from immutable BL1.0'
    }

    $approvedSelected = @($approved.dependencies | Where-Object { [bool]$_.selected })
    if ($lock.selected_packages.Count -ne 14 -or $approvedSelected.Count -ne 14 -or $vcpkg.dependencies.Count -ne 14) {
        throw 'Expected 14 selected BL1.0 packages'
    }
    $approvedQtBase = $approvedSelected | Where-Object { $_.name -eq 'qtbase' }
    $approvedQtTools = $approvedSelected | Where-Object { $_.name -eq 'qttools' }
    if ($qtContract.bl1_0_qtbase_selected_version -cne $approvedQtBase.version -or
        $qtContract.bl1_0_qttools_selected_version -cne $approvedQtTools.version) {
        throw 'Qt compatibility contract differs from immutable BL1.0 selections'
    }
    $tupleFields = @('name', 'version', 'spdx', 'official_url', 'lock', 'verification')
    for ($index = 0; $index -lt $approvedSelected.Count; $index++) {
        $lockedPackage = $lock.selected_packages[$index]
        $approvedPackage = $approvedSelected[$index]
        if ($vcpkg.dependencies[$index] -cne $lockedPackage.name) {
            throw "vcpkg manifest order/name differs at package index $index"
        }
        foreach ($field in $tupleFields) {
            if ($lockedPackage.$field -cne $approvedPackage.$field) {
                throw "Selected package $($lockedPackage.name) field '$field' differs from immutable BL1.0"
            }
        }
        $approvedHash = if ($approvedPackage.PSObject.Properties.Name -contains 'package_archive_sha256') {
            $approvedPackage.package_archive_sha256
        } else { $null }
        if ($null -eq $approvedHash) {
            if ($null -ne $lockedPackage.package_archive_sha256 -or
                $lockedPackage.package_archive_hash_state -cne 'not-defined-by-bl1.0') {
                throw "Selected package $($lockedPackage.name) lacks the required BL1.0 hash policy"
            }
        } elseif ($lockedPackage.package_archive_sha256 -cne $approvedHash) {
            throw "Selected package $($lockedPackage.name) archive hash differs from immutable BL1.0"
        }
    }

    $acquisitionContracts = @($lock.tool_acquisition_contracts)
    if ($acquisitionContracts.Count -ne 8 -or
        @($acquisitionContracts.name | Select-Object -Unique).Count -ne $acquisitionContracts.Count) {
        throw 'Expected eight unique tool acquisition contracts'
    }
    $allowedUnlockedStates = @('not-defined-by-bl1.0', 'channel-managed-not-defined-by-bl1.0')
    foreach ($contract in $acquisitionContracts) {
        if ($contract.state -eq 'locked-by-bl1.0') {
            if (-not $contract.artifact_url -or $contract.artifact_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                -not $contract.version -or -not $contract.policy) {
                throw "BL1.0 acquisition contract is incomplete for $($contract.name)"
            }
        } elseif ($contract.state -in $allowedUnlockedStates) {
            if ($null -ne $contract.artifact_url -or $null -ne $contract.artifact_sha256 -or -not $contract.policy) {
                throw "Unlocked acquisition state is not explicit for $($contract.name)"
            }
        } else { throw "Invalid acquisition contract state for $($contract.name)" }
    }

    $cudaApproved = $approved.dependencies | Where-Object { $_.name -eq 'CUDA Toolkit/cuFFT' }
    $cudaContract = $acquisitionContracts | Where-Object { $_.name -eq 'CUDA Toolkit' }
    $cudaHash = $cudaApproved.verification -replace '^sha256:', ''
    if ($cudaContract.version -cne $cudaApproved.version -or
        $cudaContract.artifact_url -cne $cudaApproved.official_url -or
        $cudaContract.artifact_sha256 -cne $cudaHash) {
        throw 'CUDA acquisition tuple differs from immutable BL1.0'
    }

    $missing = [Collections.Generic.List[object]]::new()
    $verified = [Collections.Generic.List[string]]::new()
    $cacheRoot = Join-Path $script:RepositoryRoot $cacheManifest.cache_root
    foreach ($artifact in $cacheManifest.artifacts) {
        $artifactPath = Join-Path $cacheRoot $artifact.relative_path
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            $missing.Add([ordered]@{
                id = $artifact.id
                path = $artifactPath
                url = $artifact.url
                sha256 = $artifact.sha256
                blocking = [bool]$artifact.required_for_ms00_build
            })
            continue
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifactPath).Hash.ToLowerInvariant()
        if ($actual -cne $artifact.sha256) { throw "Offline artifact hash mismatch for $($artifact.id): $actual" }
        if ($artifact.bytes -and (Get-Item -LiteralPath $artifactPath).Length -ne $artifact.bytes) {
            throw "Offline artifact size mismatch for $($artifact.id)"
        }
        $verified.Add($artifact.id)
    }
    $vcpkgArtifact = $cacheManifest.artifacts | Where-Object { $_.id -eq 'vcpkg-82b6bc8' }
    $cudaArtifact = $cacheManifest.artifacts | Where-Object { $_.id -eq 'cuda-12.8.1' }
    if ($vcpkgArtifact.relative_path -cne $approvedRelativePath -or
        $vcpkgArtifact.url -cne $lock.vcpkg.archive_url -or
        $vcpkgArtifact.sha256 -cne $lock.vcpkg.archive_sha256 -or
        [int64]$vcpkgArtifact.bytes -ne [int64]$lock.vcpkg.archive_bytes) {
        throw 'vcpkg offline artifact differs from the acquisition lock'
    }
    if ($cudaArtifact.url -cne $cudaContract.artifact_url -or
        $cudaArtifact.sha256 -cne $cudaContract.artifact_sha256) {
        throw 'CUDA offline artifact differs from the acquisition lock'
    }
    return [pscustomobject]@{
        approved_relative_path = $approvedRelativePath
        verified_offline_artifacts = @($verified)
        missing_offline_artifacts = @($missing)
        missing_material_blocks_ms00 = [bool]($missing | Where-Object { $_.blocking })
    }
}

function Get-SignalStudioDetectedHostInventory {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'CompatibleHost and ExactCapturedHost modes require Windows; use Acquisition on portable non-Windows CI.'
    }
    Import-SignalStudioMsvcEnvironment
    $cmake = Get-SignalStudioCMake
    $ninja = Initialize-SignalStudioNinjaEnvironment
    $git = Resolve-SignalStudioTool -Name 'git.exe' -Candidates @('D:\softwares\Git\cmd\git.exe')
    $python = Resolve-SignalStudioTool -Name 'python.exe'
    $cl = (Get-Command cl.exe -ErrorAction Stop).Source
    $clCommand = '"{0}" 2>&1' -f $cl
    $clBanner = (& $env:ComSpec /d /s /c $clCommand | Out-String)
    $sdkVersion = $env:WindowsSDKVersion.TrimEnd('\')
    $sdkCompiler = Join-Path $env:WindowsSdkDir "bin\$sdkVersion\x64\rc.exe"
    $tools = [Collections.Generic.List[object]]::new()
    $tools.Add((New-SignalStudioObservedTool -Name 'Git' -ToolFamily 'git' `
        -Version ((& $git --version) -replace '^git version ', '') -Path $git))
    $tools.Add((New-SignalStudioObservedTool -Name 'CMake' -ToolFamily 'cmake' `
        -Version ((& $cmake --version | Select-Object -First 1) -replace '^cmake version ', '') -Path $cmake))
    $tools.Add((New-SignalStudioObservedTool -Name 'Ninja' -ToolFamily 'ninja' -Version (& $ninja --version) -Path $ninja))
    $tools.Add((New-SignalStudioObservedTool -Name 'MSVC' -ToolFamily 'msvc' `
        -Version ([regex]::Match($clBanner, '[0-9]+\.[0-9]+\.[0-9]+').Value) -Path $cl))
    $tools.Add((New-SignalStudioObservedTool -Name 'Windows SDK' -ToolFamily 'windows-sdk' `
        -Version $sdkVersion -Path $sdkCompiler))
    $tools.Add((New-SignalStudioObservedTool -Name 'Python' -ToolFamily 'cpython' `
        -Version ((& $python --version 2>&1) -replace '^Python ', '') -Path $python))
    if (-not $Headless) {
        $qtRoot = Initialize-SignalStudioQtEnvironment
        $qmake = Join-Path $qtRoot 'bin\qmake.exe'
        if ((& $qmake -query QMAKE_XSPEC) -cne 'win32-msvc') { throw 'Qt kit does not use the MSVC ABI' }
        $tools.Add((New-SignalStudioObservedTool -Name 'Qt' -ToolFamily 'qt-msvc2022' `
            -Version (& $qmake -query QT_VERSION) -Path $qmake))
    }
    $cuda = Get-Command nvcc.exe -ErrorAction SilentlyContinue
    if ($cuda) {
        $cudaOutput = (& $cuda.Source --version | Out-String)
        $cudaVersion = [regex]::Match($cudaOutput, 'release\s+([0-9]+\.[0-9]+(?:\.[0-9]+)?)').Groups[1].Value
        $tools.Add((New-SignalStudioObservedTool -Name 'CUDA Toolkit' -ToolFamily 'cuda' `
            -Version $cudaVersion -Path $cuda.Source))
    } else {
        $tools.Add([ordered]@{
            name = 'CUDA Toolkit'
            tool_family = 'cuda'
            architecture = 'x64'
            state = 'not-detected'
            version = $null
            path = $null
            file_sha256 = $null
            hash_scope = $null
        })
    }
    return [pscustomobject][ordered]@{
        schema = 'signal-studio.current-host-evidence/1.0'
        observed_utc = [DateTime]::UtcNow.ToString('o')
        platform = 'windows'
        architecture = 'x64'
        tools = @($tools)
    }
}

function Read-SignalStudioObservedHostInventory {
    if (-not $ObservedInventoryPath) { return Get-SignalStudioDetectedHostInventory }
    $resolved = if ([IO.Path]::IsPathRooted($ObservedInventoryPath)) {
        $ObservedInventoryPath
    } else { Join-Path $script:RepositoryRoot $ObservedInventoryPath }
    $inventory = Read-SignalStudioJson -Path $resolved
    if ($inventory.schema -notin @('signal-studio.current-host-evidence/1.0',
            'signal-studio.captured-host-evidence/1.0', 'signal-studio.test-host-evidence/1.0')) {
        throw "Unsupported observed host evidence schema: $($inventory.schema)"
    }
    return $inventory
}

function Assert-SignalStudioCompatibleHost {
    param([Parameter(Mandatory = $true)][object]$Inventory)
    $contract = $lock.host_compatibility_contract
    if ($Inventory.platform -cne $contract.platform -or $Inventory.architecture -cne $contract.architecture) {
        throw "Host platform/architecture is incompatible: $($Inventory.platform)/$($Inventory.architecture)"
    }
    $requiredField = if ($Headless) { 'required_headless' } else { 'required_ui' }
    $verified = [Collections.Generic.List[string]]::new()
    foreach ($requirement in $contract.tools) {
        if (-not [bool]$requirement.$requiredField) { continue }
        $matches = @($Inventory.tools | Where-Object { $_.name -ceq $requirement.name })
        if ($matches.Count -ne 1) { throw "Observed host must contain exactly one $($requirement.name) entry" }
        $tool = $matches[0]
        if ($tool.state -cne 'detected') { throw "Required compatible tool is not detected: $($requirement.name)" }
        if ($tool.tool_family -cne $requirement.tool_family) {
            throw "Tool family mismatch for $($requirement.name): $($tool.tool_family)"
        }
        if ($tool.architecture -cne $contract.architecture) {
            throw "Tool architecture mismatch for $($requirement.name): $($tool.architecture)"
        }
        $observedVersion = ConvertTo-SignalStudioVersion -Value $tool.version -ToolName $requirement.name
        $minimum = ConvertTo-SignalStudioVersion -Value $requirement.minimum_version -ToolName $requirement.name
        $maximum = ConvertTo-SignalStudioVersion -Value $requirement.maximum_version_exclusive -ToolName $requirement.name
        if ($observedVersion -lt $minimum -or $observedVersion -ge $maximum) {
            throw "Tool version is incompatible for $($requirement.name): $($tool.version) not in [$minimum, $maximum)"
        }
        $verified.Add($requirement.name)
    }
    return @($verified)
}

function Assert-SignalStudioExactCapturedHost {
    param([Parameter(Mandatory = $true)][object]$Inventory)
    $captured = Read-SignalStudioJson -Path $capturedHostPath
    if ($captured.schema -cne 'signal-studio.captured-host-evidence/1.0') {
        throw "Unsupported captured host evidence schema: $($captured.schema)"
    }
    $requiredField = if ($Headless) { 'required_headless' } else { 'required_ui' }
    foreach ($requirement in $lock.host_compatibility_contract.tools) {
        if (-not [bool]$requirement.$requiredField) { continue }
        $expected = @($captured.tools | Where-Object { $_.name -ceq $requirement.name })
        $observed = @($Inventory.tools | Where-Object { $_.name -ceq $requirement.name })
        if ($expected.Count -ne 1 -or $observed.Count -ne 1) {
            throw "Exact captured-host evidence is incomplete for $($requirement.name)"
        }
        $expectedTool = $expected[0]
        $observedTool = $observed[0]
        if ($observedTool.version -cne $expectedTool.version -or
            $observedTool.tool_family -cne $expectedTool.tool_family -or
            $observedTool.architecture -cne $expectedTool.architecture -or
            $observedTool.file_sha256 -cne $expectedTool.file_sha256 -or
            -not (Get-SignalStudioCanonicalPathEntry -PathEntry $observedTool.path).Equals(
                (Get-SignalStudioCanonicalPathEntry -PathEntry $expectedTool.path),
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Exact captured-host mismatch for $($requirement.name)"
        }
    }
}

function Write-SignalStudioCurrentHostEvidence {
    param([Parameter(Mandatory = $true)][object]$Inventory)
    $outputPath = if ($HostEvidenceOutputPath) {
        if ([IO.Path]::IsPathRooted($HostEvidenceOutputPath)) { $HostEvidenceOutputPath }
        else { Join-Path $script:RepositoryRoot $HostEvidenceOutputPath }
    } else { Join-Path $script:RepositoryRoot 'build\host-evidence\current-host-evidence.json' }
    $directory = Split-Path -Parent $outputPath
    $null = [IO.Directory]::CreateDirectory($directory)
    $document = [ordered]@{
        schema = 'signal-studio.current-host-evidence/1.0'
        observed_utc = [DateTime]::UtcNow.ToString('o')
        platform = $Inventory.platform
        architecture = $Inventory.architecture
        source_schema = $Inventory.schema
        tools = @($Inventory.tools)
    }
    $content = ($document | ConvertTo-Json -Depth 8) + "`n"
    [IO.File]::WriteAllText($outputPath, $content, [Text.UTF8Encoding]::new($false))
    return [IO.Path]::GetFullPath($outputPath)
}

$acquisitionResult = Assert-SignalStudioAcquisitionContract
$inventory = $null
$compatibleTools = @()
$evidencePath = $null
if ($Mode -ne 'Acquisition') {
    $inventory = Read-SignalStudioObservedHostInventory
    $compatibleTools = @(Assert-SignalStudioCompatibleHost -Inventory $inventory)
    if ($Mode -eq 'ExactCapturedHost') {
        Assert-SignalStudioExactCapturedHost -Inventory $inventory
    }
    $evidencePath = Write-SignalStudioCurrentHostEvidence -Inventory $inventory
}

[ordered]@{
    schema = 'signal-studio.dependency-validation/1.0'
    mode = $Mode
    headless = [bool]$Headless
    dependency_lock_schema = $lock.schema
    vcpkg_baseline = $lock.vcpkg.builtin_baseline
    vcpkg_archive_url = $lock.vcpkg.archive_url
    vcpkg_archive_sha256 = $lock.vcpkg.archive_sha256
    verified_selected_package_tuples = $lock.selected_packages.Count
    verified_tool_acquisition_contracts = $lock.tool_acquisition_contracts.Count
    compatible_tools = $compatibleTools
    current_host_evidence = $evidencePath
    exact_captured_host = ($Mode -eq 'ExactCapturedHost')
    verified_offline_artifacts = $acquisitionResult.verified_offline_artifacts
    missing_offline_artifacts = $acquisitionResult.missing_offline_artifacts
    missing_material_blocks_ms00 = $acquisitionResult.missing_material_blocks_ms00
    policy = 'Immutable acquisition/source/package validation is always exact; compatible-host validation accepts bounded tool patches and paths; exact captured-host reproduction is opt-in.'
} | ConvertTo-Json -Depth 8
