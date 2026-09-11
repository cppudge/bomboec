# Сборка проекта: configure + build + ctest через CMake workflow preset. Использование:
#   .\build.ps1            # Release (cmake --workflow --preset release)
#   .\build.ps1 -Debug     # Debug
#   .\build.ps1 -Clean     # удалить build\ninja-<preset> перед сборкой
# conan install запускает сам CMake (cmake-conan provider, см. CMakeLists.txt). Ninja + MSVC
# нужен cl.exe в окружении: если его нет в PATH, окружение поднимается через vcvars64.
param(
    [switch]$Debug,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$preset = if ($Debug) { "debug" } else { "release" }
$binaryDir = Join-Path $PSScriptRoot "build\ninja-$preset"
if ($Clean -and (Test-Path $binaryDir)) { Remove-Item -Recurse -Force $binaryDir }

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    cmake --workflow --preset $preset
} else {
    $vcvars = & (Join-Path $PSScriptRoot "scripts\vcvars.ps1")
    cmd /c "`"$vcvars`" >nul && cmake --workflow --preset $preset"
}
exit $LASTEXITCODE
