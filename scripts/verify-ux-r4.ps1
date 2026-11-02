[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$SoftwareRenderer,
    [switch]$Baseline
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath (Join-Path $output 'report.json')) { throw 'Use a new directory to retain earlier evidence.' }
New-Item -ItemType Directory -Force $output | Out-Null
$exe=Join-Path $repo "out/vs2026-qt611-$($Configuration.ToLowerInvariant())_bin/SignalStudioUiCapture.exe"
$saved=@{}
$names=@('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS','QT_FONT_DPI','QT_DEVICE_PIXEL_RATIO','QT_AUTO_SCREEN_SCALE_FACTOR','QT_ENABLE_HIGHDPI_SCALING','QT_SCALE_FACTOR_ROUNDING_POLICY','QT_USE_PHYSICAL_DPI')
foreach($name in $names) { $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
try {
    foreach($name in $names | Where-Object {$_ -ne 'PATH'}) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
    $env:PATH="$env:SystemRoot/System32;$env:SystemRoot"
    $arguments=@('--ux-r4',$output)
    if($SoftwareRenderer){$arguments+='--software-renderer'}
    if($Baseline){$arguments+='--baseline'}
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $nativeArgs=@('--ux-r4', ('"{0}"' -f $output))
    if($SoftwareRenderer){$nativeArgs+='--software-renderer'}
    if($Baseline){$nativeArgs+='--baseline'}
    $process=Start-Process -FilePath $exe -ArgumentList $nativeArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'process.txt') -RedirectStandardError (Join-Path $output 'stderr.txt')
    if(-not $process.WaitForExit(300000)) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        throw 'Native UX-R4 runner exceeded 300 seconds; diagnostic steps and earlier scenes retained.'
    }
    $process.WaitForExit()
    $code=$process.ExitCode
    $path=Join-Path $output 'report.json'
    if(-not(Test-Path -LiteralPath $path)){throw "Native runner returned $code without a report"}
    $r=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $r | Add-Member -NotePropertyName process -NotePropertyValue ([ordered]@{
        configuration=$Configuration;exitCode=$code;elapsedSeconds=$timer.Elapsed.TotalSeconds;
        systemOnlyPath=$true;dpiOverridesCleared=$true;timeoutSeconds=300
    }) -Force
    $r | ConvertTo-Json -Depth 60 | Set-Content -LiteralPath $path -Encoding utf8
    if($code -ne 0 -or -not $r.pass){throw "UX-R4 native failure: $($r.error) after $($r.lastScene)"}
    if($r.screen.pixelSize.width -ne 3840 -or $r.screen.pixelSize.height -ne 2160 -or
       [Math]::Abs($r.screen.devicePixelRatio-1.5) -gt .001 -or
       [string]::IsNullOrWhiteSpace($r.screen.friendlyName) -or
       [string]::IsNullOrWhiteSpace($r.screen.monitorDevicePath)){throw 'Actual 4K/150% screen identity missing'}
    $expected=if($Baseline){2}else{21}
    if(@($r.scenes).Count -ne $expected){throw "Expected $expected main-window scenes"}
    foreach($scene in $r.scenes){
        if($scene.pixelSize.width -ne 3840 -or $scene.pixelSize.height -ne 2160 -or
           -not(Test-Path -LiteralPath $scene.png)){throw "Incomplete screenshot $($scene.scene)"}
        if($SoftwareRenderer){
            if($scene.backend -notmatch 'QPainter' -or $scene.render.gpuDataDrawCalls -ne 0){throw 'Software fallback evidence invalid'}
        }elseif($scene.backend -notmatch 'D3D11' -or $scene.render.gpuDataDrawCalls -le 0 -or $scene.render.textureUploads -le 0){throw 'Hardware data rendering evidence missing'}
    }
    if(-not $Baseline -and ($r.cpuDdcOutputSamples -ne 512 -or $r.fullCopyCharacters -lt 16000 -or
       $r.ordinarySupplementalEntriesVerified -ne 2 -or $r.largeRfbenchValidationExecuted -ne $false -or
       -not(Test-Path -LiteralPath $r.detailsScreenshot))){throw 'Missing full evidence copy/channel/DSP proof'}
    Write-Host "UX-R4 $Configuration software=$([bool]$SoftwareRenderer): $expected main scenes and details passed. $path"
} finally {
    foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
