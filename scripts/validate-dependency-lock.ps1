[CmdletBinding()]
param([switch]$Headless)

. (Join-Path $PSScriptRoot 'common.ps1')

$lockPath = Join-Path $script:RepositoryRoot 'dependencies\dependency-lock.json'
$cacheManifestPath = Join-Path $script:RepositoryRoot 'dependencies\offline-cache-manifest.json'
$vcpkgManifestPath = Join-Path $script:RepositoryRoot 'vcpkg.json'
foreach ($path in @($lockPath, $cacheManifestPath, $vcpkgManifestPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Dependency contract missing: $path" }
}
$lock = Get-Content -Raw -Encoding UTF8 -LiteralPath $lockPath | ConvertFrom-Json
$cacheManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $cacheManifestPath | ConvertFrom-Json
$vcpkg = Get-Content -Raw -Encoding UTF8 -LiteralPath $vcpkgManifestPath | ConvertFrom-Json
$approvedPath = Join-Path $script:RepositoryRoot $lock.approved_source
if (-not (Test-Path -LiteralPath $approvedPath -PathType Leaf)) { throw "Immutable BL1.0 dependency source missing: $approvedPath" }
$approved = Get-Content -Raw -Encoding UTF8 -LiteralPath $approvedPath | ConvertFrom-Json
$approvedFetchPath = Join-Path $script:RepositoryRoot $lock.approved_fetch_script
if (-not (Test-Path -LiteralPath $approvedFetchPath -PathType Leaf)) { throw "Immutable BL1.0 fetch script missing: $approvedFetchPath" }
$approvedFetch = Get-Content -Raw -Encoding UTF8 -LiteralPath $approvedFetchPath
$fetchAssignments = @{}
foreach ($match in [regex]::Matches($approvedFetch, '(?m)^\$(commit|url|sha)\s*=\s*"([^"]+)"\s*$')) {
    $fetchAssignments[$match.Groups[1].Value] = $match.Groups[2].Value
}
if ($fetchAssignments.Count -ne 3) { throw 'Immutable BL1.0 fetch script assignments are incomplete' }
$approvedCommit = $fetchAssignments['commit']
$approvedUrl = $fetchAssignments['url'].Replace('$commit', $approvedCommit)
$approvedSha = $fetchAssignments['sha']
$archiveMatch = [regex]::Match($approvedFetch, '(?m)^\$archive\s*=\s*Join-Path\s+\$Destination\s+"([^"]+)"\s*$')
if (-not $archiveMatch.Success) { throw 'Immutable BL1.0 fetch script archive path is missing' }
$approvedRelativePath = $archiveMatch.Groups[1].Value.Replace('$commit', $approvedCommit)
if ($lock.schema -ne 'signal-studio.dependency-lock/1.2') { throw "Unsupported dependency lock schema: $($lock.schema)" }
$qtContract = $lock.qt_compatibility_contract
if ($qtContract.minimum_supported_version -cne '6.10.3' -or
    $qtContract.ci_validation_version -cne '6.10.3' -or
    $qtContract.local_validation_version -cne '6.11.1' -or
    $qtContract.bl1_0_qtbase_selected_version -cne '6.11.1#1' -or
    $qtContract.bl1_0_qttools_selected_version -cne '6.11.1' -or
    -not $qtContract.policy) {
    throw 'Qt compatibility contract is incomplete or inconsistent'
}
if ($vcpkg.'builtin-baseline' -ne $lock.vcpkg.builtin_baseline -or $lock.vcpkg.builtin_baseline -ne $approved.vcpkg_baseline) {
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
    throw "Expected 14 selected BL1.0 packages"
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

$allowedUnlockedStates = @('not-defined-by-bl1.0', 'channel-managed-not-defined-by-bl1.0')
foreach ($tool in $lock.host_toolchain) {
    $installed = $tool.installed_instance
    $acquisition = $tool.acquisition_contract
    if ($installed.state -eq 'detected') {
        foreach ($field in @('version', 'path', 'file_sha256', 'hash_scope')) {
            if (-not $installed.$field) { throw "Detected tool $($tool.name) lacks installed-instance $field" }
        }
        if ($installed.file_sha256 -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid installed-instance hash for $($tool.name)" }
    } elseif ($installed.state -eq 'not-detected') {
        foreach ($field in @('version', 'path', 'file_sha256', 'hash_scope')) {
            if ($null -ne $installed.$field) { throw "Unavailable tool $($tool.name) asserts installed-instance $field" }
        }
    } else { throw "Invalid installed-instance state for $($tool.name)" }

    if ($acquisition.state -eq 'locked-by-bl1.0') {
        if (-not $acquisition.artifact_url -or $acquisition.artifact_sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $acquisition.policy) {
            throw "BL1.0 acquisition contract is incomplete for $($tool.name)"
        }
    } elseif ($acquisition.state -in $allowedUnlockedStates) {
        if ($null -ne $acquisition.artifact_url -or $null -ne $acquisition.artifact_sha256 -or -not $acquisition.policy) {
            throw "Unlocked acquisition state is not explicit for $($tool.name)"
        }
    } else { throw "Invalid acquisition contract state for $($tool.name)" }
}
$cudaApproved = $approved.dependencies | Where-Object { $_.name -eq 'CUDA Toolkit/cuFFT' }
$cudaLocked = $lock.host_toolchain | Where-Object { $_.name -eq 'CUDA Toolkit' }
$cudaHash = $cudaApproved.verification -replace '^sha256:', ''
if ($cudaLocked.installed_instance.state -ne 'not-detected' -or
    $cudaLocked.acquisition_contract.version -cne $cudaApproved.version -or
    $cudaLocked.acquisition_contract.artifact_url -cne $cudaApproved.official_url -or
    $cudaLocked.acquisition_contract.artifact_sha256 -cne $cudaHash) {
    throw 'CUDA installed/acquisition tuple differs from immutable BL1.0'
}

Import-SignalStudioMsvcEnvironment
$cmake = Get-SignalStudioCMake
$ninja = Initialize-SignalStudioNinjaEnvironment
$git = Resolve-SignalStudioTool -Name 'git.exe' -Candidates @('D:\softwares\Git\cmd\git.exe')
$python = Resolve-SignalStudioTool -Name 'python.exe'
$qtRoot = $null
$qmake = $null
if (-not $Headless) {
    $qtRoot = Initialize-SignalStudioQtEnvironment
    $qmake = Join-Path $qtRoot 'bin\qmake.exe'
}
$cl = (Get-Command cl.exe -ErrorAction Stop).Source
$clCommand = '"{0}" 2>&1' -f $cl
$clBanner = (& $env:ComSpec /d /s /c $clCommand | Out-String)
$observedVersions = [ordered]@{
    Git = ((& $git --version) -replace '^git version ', '')
    CMake = ((& $cmake --version | Select-Object -First 1) -replace '^cmake version ', '')
    Ninja = (& $ninja --version)
    MSVC = ([regex]::Match($clBanner, '[0-9]+\.[0-9]+\.[0-9]+').Value)
    'Windows SDK' = $env:WindowsSDKVersion.TrimEnd('\')
    Python = ((& $python --version 2>&1) -replace '^Python ', '')
}
if (-not $Headless) { $observedVersions['Qt'] = (& $qmake -query QT_VERSION) }
$observedPaths = [ordered]@{
    Git = $git
    CMake = $cmake
    Ninja = $ninja
    MSVC = $cl
    'Windows SDK' = (Join-Path $env:WindowsSdkDir "bin\$($env:WindowsSDKVersion.TrimEnd('\'))\x64\rc.exe")
    Python = $python
}
if (-not $Headless) { $observedPaths['Qt'] = $qmake }
foreach ($name in $observedVersions.Keys) {
    $entry = $lock.host_toolchain | Where-Object { $_.name -eq $name }
    if (-not $entry) { throw "Toolchain lock lacks $name" }
    if ($entry.installed_instance.version -ne $observedVersions[$name]) {
        throw "Toolchain version mismatch for ${name}: observed $($observedVersions[$name]), locked $($entry.installed_instance.version)"
    }
}
foreach ($name in $observedPaths.Keys) {
    $entry = $lock.host_toolchain | Where-Object { $_.name -eq $name }
    $observedPath = [IO.Path]::GetFullPath($observedPaths[$name]).TrimEnd('\', '/')
    $lockedPath = [IO.Path]::GetFullPath($entry.installed_instance.path).TrimEnd('\', '/')
    if (-not $observedPath.Equals($lockedPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Toolchain path mismatch for ${name}: observed $observedPath, locked $lockedPath"
    }
    $observedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $observedPath).Hash.ToLowerInvariant()
    if ($observedHash -cne $entry.installed_instance.file_sha256) {
        throw "Installed-instance hash mismatch for ${name}: observed $observedHash"
    }
}

$missing = New-Object System.Collections.Generic.List[object]
$verified = New-Object System.Collections.Generic.List[string]
$cacheRoot = Join-Path $script:RepositoryRoot $cacheManifest.cache_root
foreach ($artifact in $cacheManifest.artifacts) {
    $path = Join-Path $cacheRoot $artifact.relative_path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $missing.Add([ordered]@{ id = $artifact.id; path = $path; url = $artifact.url; sha256 = $artifact.sha256; blocking = [bool]$artifact.required_for_ms00_build })
        continue
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    if ($actual -ne $artifact.sha256) { throw "Offline artifact hash mismatch for $($artifact.id): $actual" }
    if ($artifact.bytes -and (Get-Item -LiteralPath $path).Length -ne $artifact.bytes) { throw "Offline artifact size mismatch for $($artifact.id)" }
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
if ($cudaArtifact.url -cne $cudaLocked.acquisition_contract.artifact_url -or
    $cudaArtifact.sha256 -cne $cudaLocked.acquisition_contract.artifact_sha256) {
    throw 'CUDA offline artifact differs from the acquisition lock'
}

[ordered]@{
    schema = $lock.schema
    vcpkg_baseline = $lock.vcpkg.builtin_baseline
    vcpkg_archive_url = $lock.vcpkg.archive_url
    vcpkg_archive_sha256 = $lock.vcpkg.archive_sha256
    selected_packages = $lock.selected_packages.Count
    qt_compatibility_contract = $qtContract
    observed_tool_versions = $observedVersions
    observed_tool_paths = $observedPaths
    verified_offline_artifacts = $verified
    missing_offline_artifacts = $missing
    missing_material_blocks_ms00 = [bool]($missing | Where-Object { $_.blocking })
    verified_selected_package_tuples = $lock.selected_packages.Count
    policy = 'Installed-instance hashes are host evidence. Acquisition URL/SHA fields are asserted only where immutable BL1.0 provides them; every unavailable artifact uses an explicit source-policy state.'
} | ConvertTo-Json -Depth 6
