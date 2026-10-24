[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory)]
    [string]$Project,
    [Parameter(Mandatory)]
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$package = Join-Path $repoRoot "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin"
$projectFile = Get-Item -LiteralPath $Project
if ($projectFile.PSIsContainer) {
    $candidates = @(Get-ChildItem -LiteralPath $projectFile.FullName -File -Filter '*.ssproj')
    if ($candidates.Count -ne 1) { throw 'Specify one .ssproj file; the directory must contain exactly one .ssproj.' }
    $projectFile = $candidates[0]
}
if ($projectFile.Extension -ine '.ssproj') { throw 'Explicitly import legacy v6 JSON in Signal Studio first, then pass the converted .ssproj.' }
$projectDocument = Get-Content -LiteralPath $projectFile.FullName -Raw | ConvertFrom-Json
$projectIdentity = [Guid]::Empty
if ($projectDocument.magic -ne 'SignalStudioProject' -or $projectDocument.schemaVersion -ne 1 -or
    $projectDocument.state.version -ne 6 -or
    -not [Guid]::TryParse([string]$projectDocument.projectId, [ref]$projectIdentity) -or
    $projectIdentity -eq [Guid]::Empty -or
    [string]::IsNullOrWhiteSpace($projectDocument.assetDirectory)) {
    throw 'The input must be a valid .ssproj container with v6 state and a stable projectId.'
}
$assetSource = [IO.Path]::GetFullPath([IO.Path]::Combine($projectFile.Directory.FullName, $projectDocument.assetDirectory))
if (-not (Test-Path -LiteralPath $assetSource -PathType Container)) { throw 'The project companion asset directory is missing.' }
if (-not (Test-Path -LiteralPath (Join-Path $package 'SignalStudioUiCapture.exe') -PathType Leaf)) {
    throw 'Build the native capture package first.'
}
$targetRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $targetRoot) { throw 'Use a new output directory to preserve prior evidence.' }
New-Item -ItemType Directory -Path $targetRoot | Out-Null
$environmentNames = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM',
    'QT_SCALE_FACTOR', 'QT_SCREEN_SCALE_FACTORS', 'QT_FONT_DPI', 'QT_DEVICE_PIXEL_RATIO',
    'QT_AUTO_SCREEN_SCALE_FACTOR', 'QT_ENABLE_HIGHDPI_SCALING', 'QT_SCALE_FACTOR_ROUNDING_POLICY',
    'QT_USE_PHYSICAL_DPI', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH', 'CUDA_VISIBLE_DEVICES')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$results = @()
try {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $env:QT_QPA_PLATFORM = 'windows'
    foreach ($case in @('missing-models', 'no-compatible-gpu')) {
        $caseRoot = Join-Path $targetRoot $case
        $runtime = Join-Path $caseRoot 'runtime'
        $reportDirectory = Join-Path $caseRoot 'evidence'
        New-Item -ItemType Directory -Path $runtime,$reportDirectory -Force | Out-Null
        # Dedicated copies isolate refusal checks from the user's deployed
        # package. Original model directories and DLLs are never renamed.
        foreach ($source in Get-ChildItem -LiteralPath $package -Recurse -File) {
            $relative = [IO.Path]::GetRelativePath($package, $source.FullName)
            if ($case -eq 'missing-models' -and $relative -match '^detection-models[\\/]') { continue }
            $destination = Join-Path $runtime $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source.FullName -Destination $destination
        }
        $copiedProject = Join-Path $caseRoot 'project'
        New-Item -ItemType Directory -Path $copiedProject | Out-Null
        $copiedProjectFile = Join-Path $copiedProject $projectFile.Name
        # Copy only this project's companion assets. A .ssproj can live in an
        # arbitrary shared directory or explicitly reference external assets.
        # Rewrite the test copy's asset reference so refusal probes never save
        # detection/annotation evidence back into the original project assets.
        $relativeAssets = "$($projectFile.Name).assets/$($projectDocument.projectId)"
        $copiedAssets = Join-Path $copiedProject $relativeAssets
        New-Item -ItemType Directory -Path (Split-Path -Parent $copiedAssets) | Out-Null
        Copy-Item -LiteralPath $assetSource -Destination $copiedAssets -Recurse
        $copiedDocument = Get-Content -LiteralPath $projectFile.FullName -Raw | ConvertFrom-Json
        $copiedDocument.assetDirectory = $relativeAssets
        $copiedDocument.assetMode = 'copy'
        $copiedDocument | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $copiedProjectFile -Encoding utf8
        $executable = Join-Path $runtime 'SignalStudioUiCapture.exe'
        $env:CUDA_VISIBLE_DEVICES = if ($case -eq 'no-compatible-gpu') { '-1' } else { $null }
        Push-Location -LiteralPath $runtime
        try {
            & $executable --wideband-unavailable $reportDirectory $copiedProjectFile $case | Out-Host
            $captureExitCode = $LASTEXITCODE
        } finally { Pop-Location }
        $reportPath = Join-Path $reportDirectory 'wideband-availability-report.json'
        if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { throw "No report for $case" }
        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $report | Add-Member -NotePropertyName process -NotePropertyValue @{
            executable = $executable
            binarySha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
            configuration = $Configuration
            systemOnlyPath = $true
            originalPackagePreserved = $true
            exitCode = $captureExitCode
        } -Force
        $report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $reportPath -Encoding utf8
        if ($captureExitCode -ne 0 -or $report.pass -ne $true -or
            $report.expectedFailure -ne $case -or $report.labelsManualHistoryUsable -ne $true -or
            $report.modelInferenceVerified -ne $false -or
            $report.screen.pixelSize.width -ne 3840 -or $report.screen.pixelSize.height -ne 2160 -or
            [Math]::Abs($report.screen.devicePixelRatio - 1.5) -gt 0.001 -or
            [string]::IsNullOrWhiteSpace($report.diagnostic)) {
            throw "Unavailable scenario failed: $case : $($report.error)"
        }
        $results += @{ scenario = $case; pass = $true; report = $reportPath }
    }
    @{ pass = $true; configuration = $Configuration; project = $projectFile.FullName; scenarios = $results } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $targetRoot 'unavailable-summary.json') -Encoding utf8
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
