# Установка bomboec_cable (требует прав администратора и режима тестовой подписи).
#   .\driver\install.ps1
# Доверяет тестовому сертификату, ставит драйвер и создаёт root-устройство ROOT\BomboecCable.
# Повторный запуск обновляет драйвер. Пока кабель кем-то открыт (bomboec.exe, Discord с
# микрофоном кабеля), Windows не может выгрузить старый драйвер: bomboec.exe скрипт
# останавливает сам, остальные приложения нужно закрыть, иначе новый драйвер заработает
# только после перезагрузки. В конце скрипт говорит, какой .sys на самом деле загружен.
param(
    [string]$WdkTools = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pkg = Join-Path $root "build\driver\package"
$inf = Join-Path $pkg "bomboec_cable.inf"
$sys = Join-Path $pkg "bomboec_cable.sys"
$cer = Join-Path $pkg "bomboec_test.cer"
if (-not (Test-Path $inf)) { throw "package not built: run driver\build.ps1 first" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "run from an elevated PowerShell" }

$driverVer = (Select-String -Path $inf -Pattern '^DriverVer\s*=\s*(.*)$').Matches[0].Groups[1].Value
Write-Host "package DriverVer $driverVer"

# Тестовый сертификат в доверенные корневые и издателей (нужно для test-signing).
certutil -addstore -f Root $cer | Out-Null
certutil -addstore -f TrustedPublisher $cer | Out-Null

$devcon = Join-Path $WdkTools "devcon.exe"
if (-not (Test-Path $devcon)) { throw "devcon.exe not found: $devcon" }

# Кабель занят движком: остановить, иначе старый драйвер останется до перезагрузки.
$app = Get-Process -Name bomboec -ErrorAction SilentlyContinue
if ($app) {
    Write-Host "stopping bomboec.exe (it holds the cable open)"
    $app | Stop-Process -Force
    Start-Sleep -Seconds 1
}

# Уже установлен? Тогда обновляем.
$existing = & $devcon find "ROOT\BomboecCable" 2>$null
if ($existing -match "1 matching") {
    & $devcon update $inf "ROOT\BomboecCable"
} else {
    & $devcon install $inf "ROOT\BomboecCable"
}
$rebootNeeded = $LASTEXITCODE -eq 1
if (-not $rebootNeeded -and $LASTEXITCODE -ne 0) { throw "devcon failed ($LASTEXITCODE)" }

# Что на самом деле стоит: путь образа службы и его хеш против пакета.
$image = (Get-CimInstance Win32_SystemDriver -Filter "Name='BomboecCable'").PathName
$imageFs = $image -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''
$installedHash = if ($imageFs -and (Test-Path $imageFs)) { (Get-FileHash $imageFs -Algorithm SHA256).Hash } else { "" }
$packageHash = (Get-FileHash $sys -Algorithm SHA256).Hash
Write-Host "service image: $image"
if ($installedHash -ne $packageHash) {
    Write-Warning "DriverStore still holds another bomboec_cable.sys: the package was not imported (same DriverVer?). Rebuild with driver\build.ps1 and install again."
} elseif ($rebootNeeded) {
    Write-Warning "new driver is staged, but the old one keeps running until reboot: something still holds the cable open (Discord with the cable microphone?). Close it and run install.ps1 again, or reboot."
} else {
    Write-Host "installed and running: DriverVer $driverVer"
}
Write-Host "Check Sound settings for 'Speakers (bomboec Cable)' and 'Microphone (bomboec Cable)', then start bomboec.exe again."
