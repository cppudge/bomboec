# Сборка проекта: configure + build + ctest через CMake workflow preset. Использование:
#   .\build.ps1                  # Release (cmake --workflow --preset release)
#   .\build.ps1 -Preset debug    # Debug; -Debug то же самое
#   .\build.ps1 -Preset asan     # Debug + AddressSanitizer
#   .\build.ps1 -Clean           # удалить build\ninja-<preset> перед сборкой
# conan install запускает сам CMake (cmake-conan provider, см. CMakeLists.txt). Ninja + MSVC
# нужен cl.exe в окружении: если его нет в PATH, окружение поднимается через vcvars64
# (в нём же лежит runtime ASan для тестов пресета asan).
param(
    [ValidateSet("release", "debug", "asan")]
    [string]$Preset = "release",
    [switch]$Debug,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if ($Debug) { $Preset = "debug" }
$binaryDir = Join-Path $PSScriptRoot "build\ninja-$Preset"
if ($Clean -and (Test-Path $binaryDir)) { Remove-Item -Recurse -Force $binaryDir }

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    cmake --workflow --preset $Preset
} else {
    $vcvars = & (Join-Path $PSScriptRoot "scripts\vcvars.ps1")
    cmd /c "`"$vcvars`" >nul && cmake --workflow --preset $Preset"
}
exit $LASTEXITCODE
