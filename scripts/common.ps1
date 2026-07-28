Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepositoryRoot = Split-Path -Parent $PSScriptRoot

function Get-SignalStudioCanonicalPathEntry {
    param([Parameter(Mandatory = $true)][string]$PathEntry)
    $trimmed = $PathEntry.Trim().Trim('"')
    if (-not $trimmed) { return '' }
    try {
        if ([IO.Path]::IsPathRooted($trimmed)) {
            $fullPath = [IO.Path]::GetFullPath($trimmed)
            $root = [IO.Path]::GetPathRoot($fullPath)
            $fullWithoutTrailingSeparator = $fullPath.TrimEnd('\', '/')
            $rootWithoutTrailingSeparator = if ($root) { $root.TrimEnd('\', '/') } else { '' }
            if ($root -and $fullWithoutTrailingSeparator.Equals(
                    $rootWithoutTrailingSeparator, [StringComparison]::OrdinalIgnoreCase)) {
                if ($root.EndsWith('\') -or $root.EndsWith('/')) { return $root }
                return $root + [IO.Path]::DirectorySeparatorChar
            }
            return $fullWithoutTrailingSeparator
        }
    }
    catch {
        # Preserve an unusual environment entry rather than silently dropping it.
    }
    return $trimmed.TrimEnd('\', '/')
}

function Get-SignalStudioNormalizedPath {
    param(
        [string]$PathValue = $env:Path,
        [string[]]$Exclude = @()
    )
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $excluded = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $Exclude) {
        if (-not $entry) { continue }
        $canonical = Get-SignalStudioCanonicalPathEntry -PathEntry $entry
        if ($canonical) { $null = $excluded.Add($canonical) }
    }
    $entries = [Collections.Generic.List[string]]::new()
    foreach ($entry in ($PathValue -split ';')) {
        if (-not $entry) { continue }
        $canonical = Get-SignalStudioCanonicalPathEntry -PathEntry $entry
        if (-not $canonical -or $excluded.Contains($canonical)) { continue }
        if ($seen.Add($canonical)) { $entries.Add($canonical) }
    }
    return ($entries -join ';')
}

function Add-SignalStudioPathEntry {
    param([Parameter(Mandatory = $true)][string]$PathEntry)
    $normalized = Get-SignalStudioNormalizedPath -PathValue "$PathEntry;$env:Path"
    [Environment]::SetEnvironmentVariable('Path', $normalized, 'Process')
}

function Resolve-SignalStudioTool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$Candidates = @()
    )
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    throw "Required tool '$Name' was not found. Checked: $($Candidates -join ', ')"
}

function Get-SignalStudioVsDevCmd {
    $candidates = [Collections.Generic.List[string]]::new()
    if ($env:VSINSTALLDIR) {
        $candidates.Add((Join-Path $env:VSINSTALLDIR 'Common7\Tools\VsDevCmd.bat'))
    }

    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)', 'Process')
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $installationPath = (& $vswhere -latest -version '[17.0,18.0)' -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath | Select-Object -First 1)
            if ($installationPath) {
                $candidates.Add((Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'))
            }
        }
    }

    # Keep the known developer-host location as the last-resort local fallback.
    $candidates.Add('C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Visual Studio 2022 with the MSVC x64 tools was not found. Checked: $($candidates -join ', ')"
}

function Import-SignalStudioMsvcEnvironment {
    $environmentMarker = 'vs2022-msvc-x64-v2'
    $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    $isInitialized = $compiler -and ($env:VSCMD_VER -or $env:SIGNAL_STUDIO_MSVC_ENVIRONMENT -eq $environmentMarker)
    if ($isInitialized) {
        if (($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH -cne 'x64') -or
            ($env:VSCMD_ARG_HOST_ARCH -and $env:VSCMD_ARG_HOST_ARCH -cne 'x64')) {
            throw 'The initialized MSVC environment is not x64-hosted x64.'
        }
        [Environment]::SetEnvironmentVariable('Path', (Get-SignalStudioNormalizedPath), 'Process')
        [Environment]::SetEnvironmentVariable('VSLANG', '1033', 'Process')
        [Environment]::SetEnvironmentVariable('SIGNAL_STUDIO_MSVC_ENVIRONMENT', $environmentMarker, 'Process')
        return
    }
    $vsDevCmd = Get-SignalStudioVsDevCmd
    $commandLine = "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 && set"
    $environmentLines = & $env:ComSpec /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd failed with exit code $LASTEXITCODE"
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
    [Environment]::SetEnvironmentVariable('Path', (Get-SignalStudioNormalizedPath), 'Process')
    # CMake's MSVC dependency scanner recognizes the English /showIncludes prefix.
    [Environment]::SetEnvironmentVariable('VSLANG', '1033', 'Process')
    [Environment]::SetEnvironmentVariable('SIGNAL_STUDIO_MSVC_ENVIRONMENT', $environmentMarker, 'Process')
    if ($env:VSCMD_ARG_TGT_ARCH -cne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -cne 'x64') {
        throw 'VsDevCmd did not produce an x64-hosted x64 environment.'
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'cl.exe is unavailable after importing the MSVC environment.'
    }
}

function Get-SignalStudioCMake {
    return Resolve-SignalStudioTool -Name 'cmake.exe' -Candidates @('D:\softwares\cmake\bin\cmake.exe')
}

function Get-SignalStudioCTest {
    return Resolve-SignalStudioTool -Name 'ctest.exe' -Candidates @('D:\softwares\cmake\bin\ctest.exe')
}

function Get-SignalStudioNinja {
    return Resolve-SignalStudioTool -Name 'ninja.exe' -Candidates @('D:\softwares\Qt\Tools\Ninja\ninja.exe')
}

function Initialize-SignalStudioNinjaEnvironment {
    $ninja = Get-SignalStudioNinja
    $ninjaDirectory = Split-Path -Parent $ninja
    Add-SignalStudioPathEntry -PathEntry $ninjaDirectory
    return $ninja
}

function Update-SignalStudioUserPresets {
    param(
        [Parameter(Mandatory = $true)][string]$NinjaPath,
        [string]$QtRoot = ''
    )
    if (-not $QtRoot) {
        try {
            $QtRoot = Get-SignalStudioQtRoot
        }
        catch {
            $QtRoot = ''
        }
    }
    $resolvedNinja = (Resolve-Path -LiteralPath $NinjaPath).Path
    $ninjaDirectory = Split-Path -Parent $resolvedNinja
    $resolvedQtRoot = if ($QtRoot) { (Resolve-Path -LiteralPath $QtRoot).Path.TrimEnd('\', '/') } else { '' }
    $qtBin = if ($resolvedQtRoot) { Join-Path $resolvedQtRoot 'bin' } else { '' }
    $controlledPathEntries = @($ninjaDirectory)
    if ($qtBin) { $controlledPathEntries += $qtBin }
    $basePath = Get-SignalStudioNormalizedPath -PathValue $env:Path -Exclude $controlledPathEntries
    $commonPath = if ($qtBin) {
        Get-SignalStudioNormalizedPath -PathValue "$ninjaDirectory;$qtBin;$basePath"
    } else {
        Get-SignalStudioNormalizedPath -PathValue "$ninjaDirectory;$basePath"
    }
    $basePrefixPath = Get-SignalStudioNormalizedPath -PathValue $env:CMAKE_PREFIX_PATH -Exclude @($resolvedQtRoot)
    $qtPrefixPath = if ($resolvedQtRoot) {
        Get-SignalStudioNormalizedPath -PathValue "$resolvedQtRoot;$basePrefixPath"
    } else { '' }
    $configurePresets = New-Object System.Collections.Generic.List[object]
    $buildPresets = New-Object System.Collections.Generic.List[object]
    $testPresets = New-Object System.Collections.Generic.List[object]
    $sourcePresets = @(
        'windows-msvc-headless-debug', 'windows-msvc-headless-release',
        'windows-msvc-debug', 'windows-msvc-release', 'windows-msvc-relwithdebinfo',
        'windows-msvc-cpu-debug', 'windows-msvc-cpu-release',
        'windows-msvc-cuda-debug', 'windows-msvc-cuda-release'
    )
    $commonEnvironment = [ordered]@{ PATH = $commonPath }
    foreach ($variable in @('INCLUDE', 'LIB', 'LIBPATH')) {
        $value = [Environment]::GetEnvironmentVariable($variable, 'Process')
        if ($value) { $commonEnvironment[$variable] = Get-SignalStudioNormalizedPath -PathValue $value }
    }
    foreach ($variable in @(
            'VCToolsInstallDir',
            'VCToolsRedistDir',
            'UniversalCRTSdkDir',
            'UCRTVersion',
            'WindowsSdkDir',
            'WindowsSDKVersion',
            'VSLANG')) {
        $value = [Environment]::GetEnvironmentVariable($variable, 'Process')
        if ($value) { $commonEnvironment[$variable] = $value }
    }
    $commonPrefixPath = if ($qtPrefixPath) { $qtPrefixPath } else { $basePrefixPath }
    if ($commonPrefixPath) { $commonEnvironment.CMAKE_PREFIX_PATH = $commonPrefixPath }
    $configurePresets.Add([ordered]@{
        name = 'local-msvc-toolchain-base'
        hidden = $true
        cacheVariables = [ordered]@{ CMAKE_MAKE_PROGRAM = $resolvedNinja }
        environment = $commonEnvironment
    })
    if ($resolvedQtRoot) {
        $qtEnvironment = [ordered]@{
            SIGNAL_STUDIO_QT_ROOT = $resolvedQtRoot
        }
        $configurePresets.Add([ordered]@{
            name = 'local-msvc-qt-toolchain-base'
            hidden = $true
            inherits = 'local-msvc-toolchain-base'
            cacheVariables = [ordered]@{ SIGNAL_STUDIO_QT_ROOT = $resolvedQtRoot }
            environment = $qtEnvironment
        })
    }
    foreach ($sourcePreset in $sourcePresets) {
        $localName = "local-$sourcePreset"
        $toolchainPreset = if ($resolvedQtRoot -and -not $sourcePreset.Contains('-headless-')) {
            'local-msvc-qt-toolchain-base'
        } else { 'local-msvc-toolchain-base' }
        $configurePresets.Add([ordered]@{
            name = $localName
            displayName = "Discovered local toolchain: $sourcePreset"
            inherits = @($sourcePreset, $toolchainPreset)
        })
        $buildPresets.Add([ordered]@{ name = $localName; configurePreset = $localName; jobs = 0 })
        if (-not $sourcePreset.Contains('relwithdebinfo')) {
            $testPresets.Add([ordered]@{
                name = $localName
                configurePreset = $localName
                output = [ordered]@{ outputOnFailure = $true }
                execution = [ordered]@{ noTestsAction = 'error' }
            })
        }
    }
    $document = [ordered]@{
        version = 8
        configurePresets = $configurePresets
        buildPresets = $buildPresets
        testPresets = $testPresets
    }
    $content = ($document | ConvertTo-Json -Depth 8 -Compress) + "`n"
    $path = Join-Path $script:RepositoryRoot 'CMakeUserPresets.json'
    $existing = if (Test-Path -LiteralPath $path -PathType Leaf) {
        [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
    } else { $null }
    if ($existing -cne $content) {
        $temporaryPath = Join-Path $script:RepositoryRoot ('.CMakeUserPresets.{0}.{1}.tmp' -f $PID, [guid]::NewGuid().ToString('N'))
        $backupPath = "$temporaryPath.bak"
        try {
            [IO.File]::WriteAllText($temporaryPath, $content, [Text.UTF8Encoding]::new($false))
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                [IO.File]::Replace($temporaryPath, $path, $backupPath)
            } else {
                [IO.File]::Move($temporaryPath, $path)
            }
        }
        finally {
            if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Force }
            if (Test-Path -LiteralPath $backupPath) { Remove-Item -LiteralPath $backupPath -Force }
        }
    }
    return $path
}

function Get-SignalStudioQtRoot {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:SIGNAL_STUDIO_QT_ROOT) {
        $overrideQmake = Join-Path $env:SIGNAL_STUDIO_QT_ROOT 'bin\qmake.exe'
        if (-not (Test-Path -LiteralPath $overrideQmake -PathType Leaf)) {
            throw "SIGNAL_STUDIO_QT_ROOT does not contain bin\qmake.exe: $env:SIGNAL_STUDIO_QT_ROOT"
        }
        $overrideVersion = & $overrideQmake -query QT_VERSION
        if ($LASTEXITCODE -ne 0 -or ([version]$overrideVersion -lt [version]'6.10.3') -or ([version]$overrideVersion).Major -ne 6) {
            throw "SIGNAL_STUDIO_QT_ROOT must select a Qt 6 kit at version 6.10.3 or newer: $env:SIGNAL_STUDIO_QT_ROOT"
        }
        return (Resolve-Path -LiteralPath $env:SIGNAL_STUDIO_QT_ROOT).Path
    }
    $primaryRoot = 'D:\softwares\Qt'
    if (Test-Path -LiteralPath $primaryRoot -PathType Container) {
        $kits = Get-ChildItem -LiteralPath $primaryRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^6\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending
        foreach ($kit in $kits) {
            $candidates.Add((Join-Path $kit.FullName 'msvc2022_64'))
        }
    }
    foreach ($candidate in $candidates) {
        $qmake = Join-Path $candidate 'bin\qmake.exe'
        if (Test-Path -LiteralPath $qmake -PathType Leaf) {
            $version = & $qmake -query QT_VERSION
            if ($LASTEXITCODE -eq 0 -and ([version]$version -ge [version]'6.10.3')) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    }
    throw "A Qt 6.10.3 or newer MSVC kit was not found. Set SIGNAL_STUDIO_QT_ROOT or install it under $primaryRoot."
}

function Initialize-SignalStudioQtEnvironment {
    $qtRoot = Get-SignalStudioQtRoot
    [Environment]::SetEnvironmentVariable('SIGNAL_STUDIO_QT_ROOT', $qtRoot, 'Process')
    $qtBin = Join-Path $qtRoot 'bin'
    Add-SignalStudioPathEntry -PathEntry $qtBin
    return $qtRoot
}

function Assert-SignalStudioPreset {
    param([Parameter(Mandatory = $true)][string]$Preset)
    $allowed = @(
        'windows-msvc-debug', 'windows-msvc-release', 'windows-msvc-relwithdebinfo',
        'windows-msvc-cpu-debug', 'windows-msvc-cpu-release',
        'windows-msvc-cuda-debug', 'windows-msvc-cuda-release',
        'windows-msvc-headless-debug', 'windows-msvc-headless-release'
    )
    $sourcePreset = Get-SignalStudioSourcePreset -Preset $Preset
    if ($sourcePreset -notin $allowed) {
        throw "Unknown preset '$Preset'. Allowed values: $($allowed -join ', ')"
    }
}

function Get-SignalStudioSourcePreset {
    param([Parameter(Mandatory = $true)][string]$Preset)
    if ($Preset.StartsWith('local-', [StringComparison]::Ordinal)) {
        return $Preset.Substring('local-'.Length)
    }
    return $Preset
}

function Test-SignalStudioUiPreset {
    param([Parameter(Mandatory = $true)][string]$Preset)
    return -not (Get-SignalStudioSourcePreset -Preset $Preset).Contains('-headless-')
}
