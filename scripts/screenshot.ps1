[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string[]]$Sizes = @('2560x1440'),
    [string]$OutputDirectory,
    [string]$Project,
    [switch]$Narrowband,
    [switch]$SoftwareRenderer,
    [switch]$WidebandDetection,
    [switch]$ProductionModel
)
$ErrorActionPreference = 'Stop'
function Assert-WidebandDetectionCapture($Report, [int]$CaptureExitCode, [string]$OutputDirectory) {
    if ($CaptureExitCode -ne 0 -or $Report.pass -ne $true) {
        throw "Native wideband capture failed: $($Report.error)"
    }
    if ($Report.mode -ne 'wideband-detection' -or $Report.platform -ne 'windows' -or
        $Report.fullScreen -ne $true -or $Report.screen.connectedIndex -ne 2 -or
        $Report.screen.friendlyName -notmatch '^Redmi\s*27\s*NU$' -or
        $Report.screen.devicePath -notmatch 'DISPLAY6$' -or
        $Report.screen.pixelSize.width -ne 3840 -or $Report.screen.pixelSize.height -ne 2160 -or
        [Math]::Abs($Report.screen.devicePixelRatio - 1.5) -gt 0.001 -or
        $Report.logicalWindowSize.width -ne 2560 -or $Report.logicalWindowSize.height -ne 1440) {
        throw 'Acceptance requires actual connected screen 2, Redmi 27 NU, DISPLAY6, native 4K/150% fullscreen.'
    }
    $captureScenes = @($Report.scenes)
    $expectedScenes = if ($Report.productionModelRequested -eq $true) { 25 } else { 23 }
    if ($captureScenes.Count -ne $expectedScenes -or $Report.sceneCount -ne $expectedScenes -or
        @($captureScenes.scene | Select-Object -Unique).Count -ne $expectedScenes) {
        throw "Native capture must contain all $expectedScenes unique planned scenes."
    }
    foreach ($scene in $captureScenes) {
        if ($scene.pass -ne $true -or $scene.windowFullScreen -ne $true -or
            $scene.windowScreen.connectedIndex -ne 2 -or $scene.capturedScreen.connectedIndex -ne 2 -or
            [Math]::Abs($scene.capturedDevicePixelRatio - 1.5) -gt 0.001 -or
            $scene.logicalWindowSize.width -ne 2560 -or $scene.logicalWindowSize.height -ne 1440) {
            throw "Native scene screen/DPI validation failed: $($scene.scene)"
        }
        if ($scene.captureKind -eq 'native-main-window' -and
            ($scene.capturedPixelSize.width -ne 3840 -or $scene.capturedPixelSize.height -ne 2160)) {
            throw "Main-window screenshot is not native 4K: $($scene.scene)"
        }
        if ([Math]::Abs($scene.capturedLogicalWidgetSize.width * 1.5 - $scene.capturedPixelSize.width) -gt 1 -or
            [Math]::Abs($scene.capturedLogicalWidgetSize.height * 1.5 - $scene.capturedPixelSize.height) -gt 1) {
            throw "Native widget crop does not match actual 150% DPI: $($scene.scene)"
        }
        if (-not (Test-Path -LiteralPath $scene.png -PathType Leaf) -or
            (Get-Item -LiteralPath $scene.png).Length -eq 0) {
            throw "Native PNG is absent or empty: $($scene.png)"
        }
        if (@($scene.annotationLayers).Count -ne 3 -or
            @($scene.annotationLayers | Where-Object { $_.checked -ne $true }).Count -ne 0) {
            throw "The three actual annotation layers are not enabled: $($scene.scene)"
        }
    }
    $captureCharts = @($Report.renderEvidence.charts)
    if ($captureCharts.Count -ne 3 -or
        @($captureCharts | Where-Object { $_.gpuReady -ne $true -or $_.completedFrames -le 0 }).Count -ne 0) {
        throw 'All three charts must submit actual hardware QRhi frames.'
    }
    $captureMainChart = @($captureCharts | Where-Object { $_.name -eq 'mainPlot' })
    if ($captureMainChart.Count -ne 1 -or $captureMainChart[0].textureUploads -le 0) {
        throw 'The actual IQ main chart has no uploaded data texture.'
    }
    $captureActualIqHash = (Get-FileHash -LiteralPath $Report.source.dataPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($captureActualIqHash -ne $Report.source.iqSha256 -or
        $captureActualIqHash -ne '3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254' -or
        $Report.source.sampleRateHz -ne 20000000 -or $Report.source.centerFrequencyHz -ne 2500000000 -or
        [ulong]$Report.source.physicalSamples -ne 400000 -or
        [ulong]$Report.source.decodedCompleteFrames -ne 400000 -or $Report.source.format -ne 'CF32') {
        throw 'Actual IQ identity, complete frames or SigMF Fs/Fc/format binding was not preserved.'
    }
    if ($Report.sourceCounts.importedLabels -ne 4 -or $Report.sourceCounts.manual -ne 1 -or
        $Report.sourceCounts.algorithm -ne $Report.algorithmObservationCount -or
        [ulong]$Report.rawCandidateCount -lt [ulong]$Report.algorithmObservationCount -or
        $Report.run.request.profile.id -ne 'pipeline-fixture-v1' -or
        $Report.run.lifecycle -ne 'Completed' -or [ulong]$Report.run.completedWindows -ne 3 -or
        [ulong]$Report.run.eligibleWindows -ne 3 -or
        $Report.modelInferenceVerified -ne ($Report.productionModelRequested -eq $true) -or
        $Report.zeroCandidates -ne ([ulong]$Report.rawCandidateCount -eq 0)) {
        throw 'Original/manual/actual fixture observations or completed-window facts are inconsistent.'
    }
    $captureOutputPrefix = $OutputDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $captureProjectPath = [IO.Path]::GetFullPath($Report.projectPath)
    if (-not $captureProjectPath.StartsWith($captureOutputPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $captureProjectPath -PathType Leaf) -or
        $Report.reopen.pass -ne $true -or
        $Report.reopen.annotationOriginRevisionReviewPreserved -ne $true -or
        $Report.reopen.fullSourceTextAndHashesPreserved -ne $true -or
        $Report.reopen.completedRunAndCoveragePreserved -ne $true -or
        $Report.revision.reviewStatus -ne 'Pending' -or
        $Report.revision.originalEvidenceUnchanged -ne $true -or
        $Report.revision.reviewBindsCurrentRevision -ne $true) {
        throw 'Independent persistent project/reopen or revision/review-chain acceptance failed.'
    }
}


$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'artifacts/screenshots' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($Project) { $Project = [IO.Path]::GetFullPath($Project) }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$outputDir = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$executableName = if ($WidebandDetection) { 'SignalStudioUiCapture.exe' } else { 'SignalStudio.exe' }
$executable = Join-Path $outputDir $executableName
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable does not exist: $executable" }
if ($Narrowband -and $Project) { throw 'The packaged narrowband demo cannot be combined with -Project.' }
if ($WidebandDetection -and ($Narrowband -or $SoftwareRenderer -or $Project)) {
    throw 'The wideband native scenario uses its actual IQ project and hardware renderer.'
}
$environmentNames = @(
    'PATH', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
    'QT_SCALE_FACTOR', 'QT_SCREEN_SCALE_FACTORS', 'QT_FONT_DPI',
    'QT_DEVICE_PIXEL_RATIO', 'QT_AUTO_SCREEN_SCALE_FACTOR',
    'QT_ENABLE_HIGHDPI_SCALING', 'QT_SCALE_FACTOR_ROUNDING_POLICY',
    'QT_USE_PHYSICAL_DPI', 'QT_OPENGL', 'QT_QUICK_BACKEND',
    'QSG_RHI_BACKEND', 'QT_RHI_BACKEND', 'QT_WIDGETS_RHI_BACKEND',
    'QML2_IMPORT_PATH', 'QML_IMPORT_PATH'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$renderMode = if ($SoftwareRenderer) { 'software' } else { 'gpu' }
$renderOption = if ($SoftwareRenderer) { '--software-renderer' } else { '--require-gpu' }
Push-Location -LiteralPath $outputDir
try {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $env:QT_QPA_PLATFORM = 'windows'
    if ($WidebandDetection) {
        $captureReportPath = Join-Path $OutputDirectory 'wideband-detection-report.json'
        if (Test-Path -LiteralPath $captureReportPath -PathType Leaf) {
            Remove-Item -LiteralPath $captureReportPath
        }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $captureArguments = @('--wideband-detection', $OutputDirectory)
        if ($ProductionModel) { $captureArguments += '--production-model' }
        & $executable @captureArguments | Out-Host
        $captureExitCode = $LASTEXITCODE
        if (-not (Test-Path -LiteralPath $captureReportPath -PathType Leaf)) {
            throw "Native capture produced no report; exit code $captureExitCode."
        }
        $report = Get-Content -LiteralPath $captureReportPath -Raw | ConvertFrom-Json
        $report | Add-Member -NotePropertyName process -NotePropertyValue ([ordered]@{
            executable = $executable
            configuration = $Configuration
            systemOnlyPath = $true
            dpiOverridesCleared = $true
            exitCode = $captureExitCode
            elapsedSeconds = $timer.Elapsed.TotalSeconds
            screenshotEntry = 'scripts/screenshot.ps1 -WidebandDetection'
        }) -Force
        $report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $captureReportPath -Encoding utf8
        Assert-WidebandDetectionCapture $report $captureExitCode $OutputDirectory
        Write-Host "$Configuration native wideband capture passed on screen 2 (Redmi 27 NU / DISPLAY6), 4K / 150%."
        Write-Host "Report: $captureReportPath"
        return
    }
    foreach ($size in $Sizes) {
        if ($size -notmatch '^\d+x\d+$') { throw "Invalid window size: $size" }
        $prefix = if ($Narrowband) { 'signal-studio-narrowband' } else { 'signal-studio' }
        $screenshotPath = Join-Path $OutputDirectory "$prefix-$($Configuration.ToLowerInvariant())-$renderMode-$size.png"
        $reportPath = Join-Path $OutputDirectory "$prefix-$($Configuration.ToLowerInvariant())-$renderMode-$size.json"
        $appArguments = @()
        if ($Narrowband) { $appArguments += @('--narrowband-demo', '--verify-narrowband-pages') } else { $appArguments += '--demo-data' }
        $appArguments += @('--screenshot', $screenshotPath, '--size', $size, '--screen', '2', '--full-screen', '--render-report', $reportPath, $renderOption)
        if ($size -eq '2560x1440') { $appArguments += '--verify-4k-150' }
        if ($Project) { $appArguments += @('--project', $Project) }
        & $executable @appArguments | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Screenshot failed with exit code $LASTEXITCODE" }
        if (-not (Test-Path -LiteralPath $screenshotPath) -or (Get-Item -LiteralPath $screenshotPath).Length -eq 0) {
            throw "Screenshot was not produced: $screenshotPath"
        }
        Write-Host "Screenshot saved ($renderMode / Windows platform): $screenshotPath"
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
