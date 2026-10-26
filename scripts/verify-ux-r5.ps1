[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$SoftwareRenderer,
    [switch]$MenusOnly,
    [ValidateRange(60,900)][int]$TimeoutSeconds = 480
)

# Runs an already integrated/deployed executable. Never configures or builds.
# Each GPU/software and Debug/Release invocation needs a fresh evidence folder.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) {
    if (-not (Test-Path -LiteralPath $output -PathType Container) -or
        @(Get-ChildItem -LiteralPath $output -Force).Count -ne 0) {
        throw 'Use a new or empty output directory; failed runs and earlier evidence must be retained.'
    }
} else {
    New-Item -ItemType Directory -Path $output | Out-Null
}
$output = (Get-Item -LiteralPath $output).FullName.TrimEnd([char[]]'\/')
$exe = Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioUiCapture.exe"
$reportPath = Join-Path $output 'report.json'
$launcherPath = Join-Path $output 'launcher.json'
$workingDirectory = Split-Path -Parent $exe
$expectedScenes = @(
    '01-three-source-tabs',
    '02-readonly-native-input',
    '03-tab-exclusive-overlay-select-locate',
    '04-single-convert-edit-dedup',
    '05-multiple-convert-selection',
    '06-controlled-100-conversion',
    '07-unknown-observation-conversion',
    '08-cross-view-manual-channel-ddc',
    '09-long-name-width-320',
    '10-long-name-width-650',
    '11-width-390-full-evidence-copy',
    '12-configuration-results-stale',
    '13-new-roi-publication-replaces',
    '14-durable-zero-publication',
    '15-save-reopen-no-old-run'
)
if ($MenusOnly) { $expectedScenes = @('menu-imported','menu-algorithm','menu-manual') }
$names = @(
    'PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH',
    'QT_QPA_PLATFORM','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS','QT_FONT_DPI',
    'QT_DEVICE_PIXEL_RATIO','QT_AUTO_SCREEN_SCALE_FACTOR','QT_ENABLE_HIGHDPI_SCALING',
    'QT_SCALE_FACTOR_ROUNDING_POLICY','QT_USE_PHYSICAL_DPI',
    'QT_WIDGETS_RHI_BACKEND','QSG_RHI_BACKEND','QT_QUICK_BACKEND','QT_OPENGL'
)
$saved = @{}
$present = @{}
foreach ($name in $names) {
    $present[$name] = Test-Path -LiteralPath "Env:$name"
    $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
$timer = [Diagnostics.Stopwatch]::StartNew()
$process = $null
$result = $null
$launcher = [ordered]@{
    pass = $false
    configuration = $Configuration
    softwareRenderer = [bool]$SoftwareRenderer
    binary = $exe
    workingDirectory = $workingDirectory
    outputDirectory = $output
    startedUtc = [DateTime]::UtcNow.ToString('o')
    timeoutSeconds = $TimeoutSeconds
    systemOnlyPath = $false
    dpiOverridesCleared = $false
    buildExecuted = $false
    independentReviewPerformed = $false
    modelInferenceVerified = $false
    largeRfbenchValidationExecuted = $false
}

function Save-Launcher {
    $launcher['elapsedSeconds'] = $timer.Elapsed.TotalSeconds
    $launcher | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $launcherPath -Encoding utf8
}

function Assert-ActualScreen($Screen, [string]$Context) {
    if ($null -eq $Screen -or
        $Screen.pixelSize.width -ne 3840 -or $Screen.pixelSize.height -ne 2160 -or
        $Screen.logicalGeometry.width -ne 2560 -or $Screen.logicalGeometry.height -ne 1440 -or
        [Math]::Abs([double]$Screen.devicePixelRatio - 1.5) -gt .001 -or
        [int]$Screen.connectedIndex -lt 1 -or
        [string]::IsNullOrWhiteSpace($Screen.deviceName) -or
        [string]::IsNullOrWhiteSpace($Screen.friendlyName) -or
        [string]::IsNullOrWhiteSpace($Screen.monitorDevicePath) -or
        $Screen.identityMethod -ne 'Windows active DisplayConfig source/target') {
        throw "$Context lacks the actual connected 4K/150% Windows screen identity."
    }
    # Preferred native dimensions are supplemental evidence, never a substitute
    # for the actual connected mode, Qt geometry and installed system DPR.
    if ($null -ne $Screen.preferredPixelSize -and
        ($Screen.preferredPixelSize.width -ne 3840 -or $Screen.preferredPixelSize.height -ne 2160)) {
        throw "$Context has a different preferred native screen size."
    }
}

function Assert-EvidenceFile([string]$Path, [string]$Context) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathRooted($Path)) {
        throw "$Context has no absolute evidence path."
    }
    $resolved = [IO.Path]::GetFullPath($Path)
    $root = $output + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $resolved -PathType Leaf) -or
        (Get-Item -LiteralPath $resolved).Length -eq 0) {
        throw "$Context is missing, empty or outside this invocation's evidence directory."
    }
}

try {
    Save-Launcher
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "Integrated capture executable is unavailable: $exe. Integrate the UX-R5 include/dispatch and build separately."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $workingDirectory 'SignalStudioDetectionWorker.exe') -PathType Leaf)) {
        throw 'The actual pipeline-fixture-v1 worker is missing from the deployment directory.'
    }
    foreach ($name in $names) {
        # In this PowerShell runtime a $null string argument to the .NET setter
        # leaves an empty key. Qt still reads that key (and warns for an empty
        # rounding policy). Delete via the Env provider, then verify absence.
        if ($name -ne 'PATH') {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath "Env:$name") { throw "Could not remove inherited environment key $name." }
        }
    }
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'windows'
    $launcher['systemOnlyPath'] = $true
    $launcher['dpiOverridesCleared'] = $true
    Save-Launcher
    # Quote this one Windows path argument; do not interpolate command text into
    # another shell. Launch from the deployed bin with an isolated system PATH.
    $entryMode = if ($MenusOnly) { '--ux-r5-menus' } else { '--ux-r5' }
    $nativeArgs = @($entryMode, ('"{0}"' -f $output))
    if ($SoftwareRenderer) { $nativeArgs += '--software-renderer' }
    # This is the explicitly requested visible native UI acceptance window.
    # Normal startup also permits real foreground-gated OS context clicks.
    $process = Start-Process -FilePath $exe -ArgumentList $nativeArgs -WorkingDirectory $workingDirectory `
        -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $output 'process.txt') `
        -RedirectStandardError (Join-Path $output 'stderr.txt')
    $launcher['processId'] = $process.Id
    Save-Launcher
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        $launcher['timedOut'] = $true
        throw "Native UX-R5 runner exceeded $TimeoutSeconds seconds; report checkpoints, steps and captured scenes are retained."
    }
    $process.WaitForExit()
    $code = $process.ExitCode
    $launcher['exitCode'] = $code
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "Native runner returned $code without report.json; see process.txt, stderr.txt and launcher.json."
    }
    $result = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $result | Add-Member -NotePropertyName nativePass -NotePropertyValue ([bool]$result.pass) -Force
    $expectedMode = if ($MenusOnly) { 'ux-r5-menus' } else { 'ux-r5' }
    if ($code -ne 0 -or -not $result.pass -or $result.mode -ne $expectedMode) {
        throw "UX-R5 native failure: $($result.error), scene=$($result.lastScene), step=$($result.lastStep)."
    }
    if ($result.platform -ne 'windows' -or [bool]$result.softwareRenderer -ne [bool]$SoftwareRenderer) {
        throw 'Native Windows renderer selection does not match this invocation.'
    }
    Assert-ActualScreen $result.screen 'Target screen'
    if (@($result.screenCandidates).Count -ne 1) { throw 'Acceptance screen was not unique at startup.' }
    Assert-ActualScreen $result.screenCandidates[0] 'Unique screen candidate'
    if ($result.screenCandidates[0].monitorDevicePath -ne $result.screen.monitorDevicePath) {
        throw 'Selected screen differs from the unique actual candidate.'
    }
    $scenes = @($result.scenes)
    if ($scenes.Count -ne $expectedScenes.Count) { throw 'Unexpected UX-R5 scene count for this mode.' }
    for ($index = 0; $index -lt $expectedScenes.Count; $index++) {
        $scene = $scenes[$index]
        if ($scene.scene -ne $expectedScenes[$index] -or -not $scene.pass -or $scene.status -ne 'passed') {
            throw "Incomplete, reordered or duplicate UX-R5 scene at index $index."
        }
        Assert-EvidenceFile $scene.png $scene.scene
        Assert-ActualScreen $scene.actualScreen $scene.scene
        if ($scene.actualScreen.monitorDevicePath -ne $result.screen.monitorDevicePath -or
            $scene.actualScreen.deviceName -ne $result.screen.deviceName -or
            $scene.actualScreen.connectedIndex -ne $result.screen.connectedIndex -or
            -not $scene.fullScreen -or
            $scene.logicalWindowSize.width -ne 2560 -or $scene.logicalWindowSize.height -ne 1440 -or
            $scene.pixelSize.width -ne 3840 -or $scene.pixelSize.height -ne 2160 -or
            [Math]::Abs([double]$scene.devicePixelRatio - 1.5) -gt .001 -or
            -not $scene.nativeWindowVisibleAnd4kClientVerified -or
            $scene.captureMethod -ne 'native QWidget grab with QRhi readback') {
            throw "Actual screen/window/readback evidence changed in $($scene.scene)."
        }
        if (@($scene.sourceIds).Count -eq 0 -or @($scene.rects).Count -eq 0 -or
            @($scene.charts).Count -eq 0 -or [string]::IsNullOrWhiteSpace($scene.fileId)) {
            throw "Missing source IDs, widget rectangles or chart evidence in $($scene.scene)."
        }
        if ($SoftwareRenderer) {
            if ($scene.backend -notmatch 'QPainter' -or $scene.render.gpuDataDrawCalls -ne 0) {
                throw "Software fallback proof is invalid in $($scene.scene)."
            }
        } elseif ($scene.backend -notmatch 'D3D11' -or $scene.render.gpuDataDrawCalls -le 0 -or
                  $scene.render.gpuHeatmapDrawCalls -le 0 -or $scene.render.textureUploads -le 0) {
            throw "Hardware scalar-data draw/upload proof is missing in $($scene.scene)."
        }
        foreach ($chart in @($scene.charts)) {
            if ($SoftwareRenderer) {
                if ($chart.backend -notmatch 'QPainter' -or $chart.render.gpuDataDrawCalls -ne 0) {
                    throw "Visible software chart evidence is invalid: $($chart.objectName)."
                }
            } elseif ($chart.backend -notmatch 'D3D11' -or $chart.render.gpuDataDrawCalls -le 0) {
                throw "Visible GPU chart lacks real data draws: $($chart.objectName)."
            }
        }
    }
    if ($MenusOnly) {
        if (-not $result.menuRegressionVerified -or $result.fullJourneyExecuted -ne $false) { throw 'Missing targeted menu proof' }
        foreach ($scene in $scenes) {
            $editable = $scene.facts.sourceKind -eq 1
            if (-not $scene.facts.sourceUnchanged) { throw 'Native menu changed a source' }
            foreach ($popup in @($scene.facts.popups.global,$scene.facts.popups.tree,$scene.facts.popups.graph)) {
                Assert-EvidenceFile $popup.png 'Actual native menu popup'
                Assert-ActualScreen $popup.actualScreen 'Actual menu screen'
                if ([bool]$popup.renameEnabled -ne $editable -or [bool]$popup.deleteEnabled -ne $editable) { throw 'Native menu capability mismatch' }
            }
        }
    } else {
    if ($result.cpuDdcOutputSamples -ne 512 -or -not $result.manualChannelSnapshotReopened -or
        -not $result.saveReopenVerified -or -not $result.noRetiredRunDiscovery -or
        $result.fullCopyCharacters -le 16000 -or $result.ordinarySupplementalEntriesVerified -ne 2 -or
        $result.largeRfbenchValidationExecuted -ne $false -or $result.modelInferenceVerified -ne $false -or
        $result.fieldRfAccuracyVerified -ne $false -or $result.independentReviewPerformed -ne $false) {
        throw 'Missing channel/DDC/reopen/evidence-copy proof or incorrect coverage claims.'
    }
    if ($result.labelImport.automaticCountVerified -ne 4 -or
        @($result.labelImport.actualImportedIds).Count -ne 4 -or
        -not $result.labelImport.explicitRepeatImportIdempotent -or
        -not $result.labelImport.sourceImportsOutsideManualUndo -or
        $result.labelImport.explicitFallbackAttempted -ne $false) {
        throw 'Ordinary four-label automatic discovery, idempotent repeat import or source/undo isolation was not verified.'
    }
    if ($scenes[5].facts.count -ne 100 -or -not $scenes[5].facts.atomicUndoRedo -or -not $scenes[5].facts.deduplicated -or
        @($scenes[5].facts.sourceIds).Count -ne 100 -or @($scenes[5].facts.manualIds).Count -ne 100 -or
        -not $scenes[6].facts.activityTimeStillUnknown -or $scenes[6].facts.activitySegments -ne 0 -or
        $scenes[8].facts.propertyWidth -ne 320 -or $scenes[9].facts.propertyWidth -ne 650 -or
        $scenes[10].facts.geometry.propertyWidth -ne 390 -or
        $scenes[13].facts.canonicalReplayCount -ne 0 -or
        $scenes[14].facts.bulkManualCopiesReopened -ne 100 -or
        -not $scenes[14].facts.ddcSamplesEqualWithoutHashing) {
        throw 'Core conversion/unknown-time/resize/zero-publication assertions are incomplete.'
    }
    foreach ($entry in @($result.firstPublication, $scenes[12].facts, $scenes[13].facts, $scenes[14].facts)) {
        if (-not $entry.manifestAndChunksVerifiedByProductStore -or -not $entry.singleAuthoritySlot -or
            $entry.run.lifecycle -ne 'Completed' -or @($entry.run.chunks).Count -eq 0) {
            throw 'A claimed formal publication lacks a complete product-verified durable manifest/chunk set.'
        }
    }
    Assert-EvidenceFile $result.projectPath 'Saved native project'
    Assert-EvidenceFile $result.channelDialogScreenshot 'Manual channel configuration'
    foreach ($entry in @($scenes[10].facts.primary, $scenes[10].facts.supplementalInspector, $scenes[10].facts.supplementalAuxiliary)) {
        Assert-EvidenceFile $entry.png 'Full original evidence details'
        Assert-ActualScreen $entry.actualScreen 'Evidence details screen'
        if (-not $entry.completePrimary -or -not $entry.sourceHashAndPathPresent) { throw 'Incomplete original evidence copy.' }
    }
    }
    $launcher['pass'] = $true
    $result | Add-Member -NotePropertyName scriptVerification -NotePropertyValue ([ordered]@{
        pass = $true; coreScenes = $scenes.Count; actualScreenChecked = $true
        sourceIdsAndRectsChecked = $true; rendererDrawsAndUploadsChecked = $true
        independentReviewPerformed = $false
    }) -Force
    Write-Host "UX-R5 $Configuration software=$([bool]$SoftwareRenderer) mode=$expectedMode : $($scenes.Count) native scenes checked. $reportPath"
} catch {
    $failure = $_.Exception.Message
    $launcher['error'] = $failure
    if ($null -ne $result) {
        $result.pass = $false
        $result | Add-Member -NotePropertyName scriptVerification -NotePropertyValue ([ordered]@{
            pass = $false; error = $failure; independentReviewPerformed = $false
        }) -Force
    }
    throw
} finally {
    foreach ($name in $names) {
        if ($present[$name]) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
        else { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    }
    $launcher['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    Save-Launcher
    if ($null -ne $result) {
        $result | Add-Member -NotePropertyName process -NotePropertyValue $launcher -Force
        $result | ConvertTo-Json -Depth 90 | Set-Content -LiteralPath $reportPath -Encoding utf8
    }
    if ($null -ne $process) { $process.Dispose() }
}
