$ErrorActionPreference='Stop'
Set-Location -LiteralPath 'D:/project/signal_studio_rebuild'
$taskRoot=[IO.Path]::GetFullPath('out/tasks/wideband-detection/UX-R3-FEEDBACK-01')
$cases=@()
foreach($configuration in @('Debug','Release')) {
  foreach($software in @($false,$true)) {
    foreach($narrowband in @($false,$true)) {
      $mode=if($software){'software'}else{'gpu'}
      $view=if($narrowband){'five-pages'}else{'wideband'}
      $name="package-final-$configuration-$view-$mode"
      & scripts/verify-package.ps1 -Configuration $configuration -SoftwareRenderer:$software -Narrowband:$narrowband -ReportPath (Join-Path $taskRoot "diagnostics/$name.json") *> (Join-Path $taskRoot "logs/$name.txt")
      if(-not $?){throw "Failed: $name"}
      $cases+=@{name=$name;pass=$true}
    }
  }
  foreach($axis in @('A','B','C')) {
    $name="cli-$configuration-$axis"
    & scripts/verify-package.ps1 -Configuration $configuration -SoftwareRenderer -AxisTitleMode $axis -ReportPath (Join-Path $taskRoot "diagnostics/$name.json") *> (Join-Path $taskRoot "logs/$name.txt")
    if(-not $?){throw "Failed: $name"}
    $cases+=@{name=$name;pass=$true}
  }
}
@{status='passed';cases=$cases;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $taskRoot 'diagnostics/startup-final-status.json') -Encoding utf8
