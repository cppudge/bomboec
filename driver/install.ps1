# Установка bomboec_cable (требует прав администратора и режима тестовой подписи).
#   .\driver\install.ps1
# Доверяет тестовому сертификату, ставит драйвер и создаёт root-устройство ROOT\BomboecCable.
param(
    [string]$WdkTools = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pkg = Join-Path $root "build\driver\package"
$inf = Join-Path $pkg "bomboec_cable.inf"
$cer = Join-Path $pkg "bomboec_test.cer"
if (-not (Test-Path $inf)) { throw "package not built: run driver\build.ps1 first" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "run from an elevated PowerShell" }

# Тестовый сертификат в доверенные корневые и издателей (нужно для test-signing).
certutil -addstore -f Root $cer | Out-Null
certutil -addstore -f TrustedPublisher $cer | Out-Null

$devcon = Join-Path $WdkTools "devcon.exe"
if (-not (Test-Path $devcon)) { throw "devcon.exe not found: $devcon" }

# Уже установлен? Тогда обновляем.
$existing = & $devcon find "ROOT\BomboecCable" 2>$null
if ($existing -match "1 matching") {
    & $devcon update $inf "ROOT\BomboecCable"
} else {
    & $devcon install $inf "ROOT\BomboecCable"
}
if ($LASTEXITCODE -eq 1) { Write-Warning "devcon: reboot required to finish the update (device was in use)" }
elseif ($LASTEXITCODE -ne 0) { throw "devcon failed ($LASTEXITCODE)" }
Write-Host "installed. Check Sound settings for 'Speakers (bomboec Cable)' and 'Microphone (bomboec Cable)'."
