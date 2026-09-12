# Установка bomboec_cable (требует прав администратора и режима тестовой подписи).
#   .\driver\install.ps1 [-Force]
# Доверяет тестовому сертификату, ставит драйвер и создаёт root-устройство ROOT\BomboecCable.
# Повторный запуск обновляет драйвер, только если .sys пакета отличается от установленного
# (-Force: всегда). Windows при каждом реальном обновлении драйвера удаляет endpoint'ы кабеля и
# создаёт новые с другими id (setupapi.dev.log: «Delete Device SWD\MMDEVAPI\{...}» сразу после
# «Restarting device»): приложения, помнящие микрофон по id (Discord, OBS), теряют выбор, и
# скрипт об этом предупреждает; bomboec находит устройства по имени сам.
# Пока кабель кем-то открыт (bomboec.exe, Discord с микрофоном кабеля), Windows не может
# выгрузить старый драйвер: bomboec.exe скрипт останавливает сам, остальных перечисляет через
# bomboec-sessions.exe из сборки и останавливается, чтобы их закрыли (-Force: ставит всё равно,
# тогда новый драйвер заработает после перезагрузки). В конце скрипт говорит, какой .sys на
# самом деле загружен.
param(
    [string]$WdkTools = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64",
    [switch]$Force
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

# Хеш .sys, который на самом деле стоит (путь образа службы), пусто без службы.
function Get-InstalledHash {
    $image = (Get-CimInstance Win32_SystemDriver -Filter "Name='BomboecCable'" -ErrorAction SilentlyContinue).PathName
    $imageFs = $image -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''
    if ($imageFs -and (Test-Path $imageFs)) { return @{ Path = $image; Hash = (Get-FileHash $imageFs -Algorithm SHA256).Hash } }
    return @{ Path = $image; Hash = "" }
}

# Активные endpoint'ы кабеля: "имя = id" в формате id MMDevice (как в bomboec.state.toml).
function Get-CableEndpoints {
    $out = @()
    foreach ($flow in "Capture", "Render") {
        $base = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow"
        foreach ($key in Get-ChildItem $base -ErrorAction SilentlyContinue) {
            $p = Get-ItemProperty "$($key.PSPath)\Properties" -ErrorAction SilentlyContinue
            if (-not $p -or $p.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6' -ne 'bomboec Cable') { continue }
            if ((Get-ItemProperty $key.PSPath).DeviceState -ne 1) { continue }
            $prefix = if ($flow -eq "Capture") { "{0.0.1.00000000}" } else { "{0.0.0.00000000}" }
            $out += "$($p.'{a45c254e-df1c-4efd-8020-67d146a850e0},2') (bomboec Cable) = $prefix.$($key.PSChildName)"
        }
    }
    return $out
}

$packageHash = (Get-FileHash $sys -Algorithm SHA256).Hash
$installed = Get-InstalledHash
if ($installed.Hash -eq $packageHash -and -not $Force) {
    Write-Host "installed driver already matches the package: nothing to do (endpoint ids stay). Use -Force to reinstall anyway."
    exit 0
}

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

# Кто ещё держит кабель: Windows не выгрузит драйвер, пока они не закроют поток.
$sessionsTool = Join-Path $root "build\ninja-release\bin\bomboec-sessions.exe"
if (Test-Path $sessionsTool) {
    $holders = & $sessionsTool --filter "bomboec Cable" 2>$null
    if ($LASTEXITCODE -eq 1) {
        Write-Warning "the cable is still open by:`n$($holders -join "`n")"
        if (-not $Force) { throw "close them and run install.ps1 again (or -Force: the new driver then waits for a reboot)" }
    }
}

$before = Get-CableEndpoints

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
$installed = Get-InstalledHash
Write-Host "service image: $($installed.Path)"
if ($installed.Hash -ne $packageHash) {
    Write-Warning "DriverStore still holds another bomboec_cable.sys: the package was not imported (same DriverVer?). Rebuild with driver\build.ps1 and install again."
} elseif ($rebootNeeded) {
    Write-Warning "new driver is staged, but the old one keeps running until reboot: something still holds the cable open (Discord with the cable microphone?). Close it and run install.ps1 again, or reboot."
} else {
    Write-Host "installed and running: DriverVer $driverVer"
}

# Endpoint builder пересоздаёт endpoint'ы через полсекунды после рестарта устройства.
Start-Sleep -Seconds 2
$after = Get-CableEndpoints
if ($before.Count -gt 0 -and (Compare-Object $before $after)) {
    Write-Warning ("Windows re-created the cable endpoints with new ids (it does so on every driver update):`n  before: " +
        ($before -join "`n          ") + "`n  after:  " + ($after -join "`n          ") +
        "`nApplications that remember the microphone by id (Discord, OBS) show the old one as missing: re-select 'Microphone (bomboec Cable)' there. bomboec finds its devices by name itself.")
} else {
    Write-Host "cable endpoints:`n  $($after -join "`n  ")"
}
Write-Host "Check Sound settings for 'Speakers (bomboec Cable)' and 'Microphone (bomboec Cable)', then start bomboec.exe again."
