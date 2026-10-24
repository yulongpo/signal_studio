[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Release',
      [Parameter(Mandatory)][string]$AnalysisProject,
      [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$destination=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $destination|Out-Null
$cases=@()
foreach($software in @($false,$true)){
    foreach($mode in @('wideband','five-pages','real-eye-project')){
        $name="$mode-$(if($software){'software'}else{'gpu'})"
        $report=Join-Path $destination "$name.json"
        if(Test-Path -LiteralPath $report){throw "Use a fresh evidence directory: $report"}
        $arguments=@{Configuration=$Configuration;SoftwareRenderer=$software;ReportPath=$report}
        if($mode -eq 'five-pages'){$arguments.Narrowband=$true}
        if($mode -eq 'real-eye-project'){$arguments.AnalysisProject=$AnalysisProject}
        & "$PSScriptRoot/verify-package.ps1" @arguments *> (Join-Path $destination "$name.log")
        if(-not $?){throw "Deployment case failed: $name"}
        $actual=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
        if($actual.pass -ne $true -or $actual.standaloneProcess.systemOnlyPath -ne $true){throw "Deployment report failed: $name"}
        $cases+=@{case=$name;report=$report;pass=$true}
    }
}
@{schemaVersion=1;pass=$true;configuration=$Configuration;cases=$cases}|ConvertTo-Json -Depth 5|
    Set-Content -LiteralPath (Join-Path $destination 'deployment-summary.json') -Encoding utf8
