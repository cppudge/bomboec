# Локальный CI (у репозитория нет remote): сборка и тесты всех пресетов, затем линт.
#   pwsh scripts/ci.ps1                            # release, debug, asan и check.ps1
#   pwsh scripts/ci.ps1 -Presets release,asan -NoLint
# Останавливается на первой неудаче, код выхода 1. Первая сборка пресета собирает
# зависимости Conan в .conan2 (debug и asan делят одни и те же, несколько минут).
param(
    [string[]]$Presets = @("release", "debug", "asan"),
    [switch]$NoLint
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$results = [ordered]@{}
$failed = $false

foreach ($preset in $Presets) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & pwsh -NoProfile -File (Join-Path $root "build.ps1") -Preset $preset
    $ok = $LASTEXITCODE -eq 0
    $results[$preset] = if ($ok) { "ok ($([int]$sw.Elapsed.TotalSeconds) s)" } else { "FAILED" }
    if (-not $ok) { $failed = $true; break }
}
if (-not $failed -and -not $NoLint) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot "check.ps1")
    $results["lint"] = if ($LASTEXITCODE -eq 0) { "ok" } else { "FAILED" }
    $failed = $LASTEXITCODE -ne 0
}

Write-Host ""
$results.GetEnumerator() | ForEach-Object { Write-Host ("{0,-8} {1}" -f $_.Key, $_.Value) }
if ($failed) { exit 1 }
