[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [ValidateRange(10, 3600)]
    [int]$IntervalSeconds = 60
)

$ErrorActionPreference = "Stop"
$resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
$parent = [IO.Path]::GetDirectoryName($resolvedOutput)
[IO.Directory]::CreateDirectory($parent) | Out-Null
$encoding = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText(
    $resolvedOutput,
    "timestamp,process_id,cpu_seconds,working_set_bytes,peak_working_set_bytes,private_bytes,handles,status`r`n",
    $encoding)

while ($true) {
    $process = Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
    $timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    if ($null -eq $process) {
        [IO.File]::AppendAllText(
            $resolvedOutput,
            "$timestamp,$TargetProcessId,,,,,,ENDED`r`n",
            $encoding)
        break
    }
    $cpu = $process.CPU.ToString("0.000000", [Globalization.CultureInfo]::InvariantCulture)
    $line = "$timestamp,$TargetProcessId,$cpu,$($process.WorkingSet64),$($process.PeakWorkingSet64)," +
        "$($process.PrivateMemorySize64),$($process.HandleCount),RUNNING`r`n"
    [IO.File]::AppendAllText($resolvedOutput, $line, $encoding)
    Start-Sleep -Seconds $IntervalSeconds
}
