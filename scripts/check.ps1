# clang-format и clang-tidy по src/ и tests/. Использование:
#   pwsh scripts/check.ps1              # проверить; код выхода 1, если есть замечания
#   pwsh scripts/check.ps1 -Fix         # clang-format -i и автоисправления clang-tidy
#   pwsh scripts/check.ps1 -FormatOnly  # только clang-format
# clang-tidy читает базу компиляции пресета release: сначала .\build.ps1 или cmake --preset release.
param(
    [switch]$Fix,
    [switch]$FormatOnly,
    [string]$BuildDir = "build/ninja-release"
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

function Find-LlvmTool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $path = Join-Path $env:ProgramFiles "LLVM\bin\$name.exe"
    if (Test-Path $path) { return $path }
    throw "$name not found: install LLVM or add it to PATH"
}

$failed = $false

if (-not $FormatOnly) {
    $db = Join-Path $BuildDir "compile_commands.json"
    if (-not (Test-Path $db)) { throw "$db not found: configure first (cmake --preset release)" }
    $clangTidy = Find-LlvmTool "clang-tidy"
    $units = git ls-files 'src/*.cpp' 'tests/*.cpp'
    # Исправления собираются по всем файлам и применяются одним clang-apply-replacements:
    # параллельные --fix правили бы общие заголовки наперебой.
    $fixDir = if ($Fix) { New-Item -ItemType Directory -Force (Join-Path ([IO.Path]::GetTempPath()) "bomboec-tidy-fixes-$PID") }
    $results = $units | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
        $extra = if ($using:fixDir) {
            "--export-fixes=$(Join-Path $using:fixDir (($_ -replace '[\\/]', '_') + '.yaml'))"
        } else { "--warnings-as-errors=*" }
        $out = & $using:clangTidy -p $using:BuildDir --quiet $extra $_ 2>&1
        [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($out | ForEach-Object { "$_" }) }
    }
    # Замечания в заголовках приходят от каждого включающего их файла: печатаем уникальные.
    $diags = $results.Lines | Where-Object { $_ -match ': (warning|error): ' } | Sort-Object -Unique
    $diags | ForEach-Object { Write-Host $_ }
    if ($Fix) {
        & (Find-LlvmTool "clang-apply-replacements") $fixDir.FullName
        Remove-Item -Recurse -Force $fixDir.FullName
        Write-Host "clang-tidy: applied available fixes for $($diags.Count) findings"
    } elseif ($diags.Count -gt 0 -or ($results | Where-Object Code -ne 0)) {
        Write-Host "clang-tidy: $($diags.Count) findings"
        $failed = $true
    } else {
        Write-Host "clang-tidy: clean ($($units.Count) files)"
    }
}

$clangFormat = Find-LlvmTool "clang-format"
$sources = git ls-files 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h'
if ($Fix) {
    & $clangFormat -i @sources
} else {
    & $clangFormat --dry-run --Werror @sources
    if ($LASTEXITCODE -ne 0) { $failed = $true } else { Write-Host "clang-format: clean ($($sources.Count) files)" }
}

if ($failed) { exit 1 }
