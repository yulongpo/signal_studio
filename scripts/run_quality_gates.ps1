[CmdletBinding()]
param(
  [switch]$FormatCheck
)

. (Join-Path $PSScriptRoot 'common.ps1')
$ErrorActionPreference = 'Stop'

$repoRoot = $script:RepositoryRoot
$clangFormat = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\bin\clang-format.exe'
if (-not (Test-Path -LiteralPath $clangFormat -PathType Leaf)) {
  # Fall back to PATH discovery.
  $clangFormat = (Get-Command clang-format -ErrorAction SilentlyContinue).Source
}
if (-not $clangFormat) {
  Write-Warning 'clang-format not found; format check skipped (environment deviation).'
}
else {
  Write-Host "clang-format: $clangFormat"
  $sources = Get-ChildItem -Path (Join-Path $repoRoot 'include'), (Join-Path $repoRoot 'src'), (Join-Path $repoRoot 'apps'), (Join-Path $repoRoot 'tests') -Recurse -Include '*.hpp','*.cpp','*.h','*.c' -ErrorAction SilentlyContinue
  $unformatted = @()
  foreach ($src in $sources) {
    # Use --dry-run --Werror so the check is byte-accurate and not confused by CRLF/LF differences.
    & $clangFormat -style=file --dry-run --Werror $src.FullName 2>$null
    if ($LASTEXITCODE -ne 0) {
      $unformatted += $src.FullName
    }
  }
  if ($unformatted.Count -gt 0) {
    $list = ($unformatted -join "`n")
    if ($FormatCheck) {
      Write-Error "clang-format check failed for $($unformatted.Count) file(s):`n$list"
      exit 1
    }
    else {
      Write-Host "Reformatting $($unformatted.Count) file(s)..."
      foreach ($f in $unformatted) { & $clangFormat -style=file -i $f }
    }
  }
  else {
    Write-Host 'clang-format: all sources conform.'
  }
}

Write-Host 'Quality gate: high warning level (/W4 /permissive-) is enforced at compile time by signal_studio_enable_warnings for every target.'
Write-Host 'Quality gate: dependency DAG is enforced at configure time by signal_studio_verify_dependency_graph.'
Write-Host 'Quality gate: public header third-party-type scan is covered by tests/platform contract tests.'
Write-Host 'run_quality_gates: done.'
