# Сборка проекта: conan install + cmake. Использование:
#   .\build.ps1            # Release
#   .\build.ps1 -Debug     # Debug
#   .\build.ps1 -Clean     # удалить build/ перед сборкой
param(
    [switch]$Debug,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$profile = if ($Debug) { "./conan/profiles/msvc-debug" } else { "./conan/profiles/msvc-release" }
$preset  = if ($Debug) { "conan-debug" } else { "conan-release" }

if ($Clean -and (Test-Path build)) { Remove-Item -Recurse -Force build }

# Локальный индекс рецептов (webrtc-audio-processing и другие, которых нет в Conan Center).
$remotes = conan remote list 2>$null
if (-not ($remotes -match "^bomboec-local:")) {
    conan remote add bomboec-local ./conan-recipes --type local-recipes-index
}

# Только нужные remotes: пользовательские корпоративные remotes могут быть недоступны.
conan install . -pr:a $profile -r bomboec-local -r conancenter --build=missing
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Ninja + MSVC: окружение компилятора поднимает conanbuild.bat, поэтому cmake запускаем через cmd.
$gen = if ($Debug) { "build\Debug\generators" } else { "build\Release\generators" }
cmd /c "$gen\conanbuild.bat && cmake --preset $preset && cmake --build --preset $preset"
exit $LASTEXITCODE
