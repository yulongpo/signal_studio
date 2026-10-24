[CmdletBinding()]
param([Parameter(Mandatory)][string]$Project,[Parameter(Mandatory)][string]$ReportPath)
$ErrorActionPreference='Stop'
$projectFile=(Resolve-Path -LiteralPath $Project).Path
if([IO.Path]::GetExtension($projectFile) -ine '.ssproj'){throw 'Choose an existing .ssproj project'}
$program='HKCU:\Software\Classes\SignalStudio.Project.1'
if(Test-Path -LiteralPath $program){throw 'An existing registration must be preserved; controlled association test requires an absent SignalStudio.Project.1 key'}
$expectedExe=(Resolve-Path -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'out/vs2026-qt611-release_bin/SignalStudio.exe')).Path
$expectedName=(Get-Content -LiteralPath $projectFile -Raw | ConvertFrom-Json).state.name
if([string]::IsNullOrWhiteSpace($expectedName)){throw 'The selected project has no name'}
$report=[ordered]@{schemaVersion=1;pass=$false;project=$projectFile;executable=$expectedExe;registeredCommand=$null;openedProcessId=$null;windowTitle=$null;unregistered=$false;error=$null}
$launched=$null
try{
    & "$PSScriptRoot/ssproj-association.ps1" -Executable $expectedExe
    if(-not $?){throw 'Association registration failed'}
    $command=(Get-Item -LiteralPath "$program\shell\open\command").GetValue('')
    $report.registeredCommand=$command
    if($command -cne ('"{0}" --project "%1"' -f $expectedExe)){throw 'The shell command does not quote the executable and project argument'}
    # ShellExecute uses the registered document handler, as Explorer does.
    $launched=Start-Process -FilePath $projectFile -WindowStyle Hidden -PassThru
    $report.openedProcessId=$launched.Id
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do{
        Start-Sleep -Milliseconds 100
        $launched.Refresh()
        if($launched.HasExited){throw 'The associated application exited before opening its project'}
    }until(($launched.MainWindowHandle -ne 0 -and $launched.MainWindowTitle.Contains($expectedName)) -or [DateTime]::UtcNow -ge $deadline)
    $report.windowTitle=$launched.MainWindowTitle
    $actual=Get-CimInstance Win32_Process -Filter "ProcessId=$($launched.Id)"
    if($actual.ExecutablePath -ine $expectedExe -or $actual.CommandLine -notlike "*--project*${projectFile}*") {throw 'ShellExecute did not launch the expected deployed executable with the selected project'}
    if($launched.MainWindowHandle -eq 0 -or [string]::IsNullOrWhiteSpace($report.windowTitle)){throw 'The project window did not open'}
    if(-not $report.windowTitle.Contains($expectedName)){throw 'The window title does not identify the selected project'}
    if($report.windowTitle -like '*打开失败*'){throw 'Project open failed'}
    $report.pass=$true
}catch{$report.error=$_.Exception.Message;throw}
finally{
    if($launched -and -not $launched.HasExited){
        [void]$launched.CloseMainWindow()
        if(-not $launched.WaitForExit(10000)){$report.pass=$false;$report.error='The owned association-test window did not close; left running for inspection'}
    }
    & "$PSScriptRoot/ssproj-association.ps1" -Unregister
    $report.unregistered=-not (Test-Path -LiteralPath $program)
    $report.pass=$report.pass -and $report.unregistered
    $absoluteReport=[IO.Path]::GetFullPath($ReportPath)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $absoluteReport)|Out-Null
    $report|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $absoluteReport -Encoding utf8
}
if(-not $report.pass){throw 'Controlled association test failed; inspect the report'}
