param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = "Stop"
$excel = New-Object -ComObject Excel.Application
$excel.Visible = $false
$excel.DisplayAlerts = $false
try {
    Get-ChildItem -LiteralPath $Root -Recurse -Filter *.xlsx | ForEach-Object {
        $book = $excel.Workbooks.Open($_.FullName, 0, $false)
        try {
            $excel.CalculateFullRebuild()
            $book.Save()
        } finally {
            $book.Close($true)
            [System.Runtime.InteropServices.Marshal]::ReleaseComObject($book) | Out-Null
        }
        Write-Output "Recalculated $($_.FullName)"
    }
} finally {
    $excel.Quit()
    [System.Runtime.InteropServices.Marshal]::ReleaseComObject($excel) | Out-Null
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
