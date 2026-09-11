# Сборка и тестовая подпись драйвера bomboec_cable. Запускать из pwsh 7 (Cert: drive).
#   .\driver\build.ps1            # собрать, создать .cat, подписать тестовым сертификатом
#   .\driver\build.ps1 -Clean
# Результат: build\driver\package\{bomboec_cable.sys, bomboec_cable.inf, bomboec_cable.cat, bomboec_test.cer}
# Установка требует прав администратора: driver\install.ps1.
param(
    [switch]$Clean,
    [string]$VsRoot,  # по умолчанию последняя Visual Studio с C++ (vswhere)
    [string]$WdkBin = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build\driver"
$pkg = Join-Path $buildDir "package"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

$vcvars = if ($VsRoot) { Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat" } else { & (Join-Path $root "scripts\vcvars.ps1") }
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

$src = Join-Path $root "driver"
cmd /c "`"$vcvars`" >nul && cmake -S `"$src`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { throw "driver build failed" }

# DriverVer в пакете: дата и монотонная версия сборки. С неизменной DriverVer Windows
# считает пакет уже импортированным ("already imported as oemNN.inf") и при переустановке
# оставляет старый .sys в DriverStore, ничего не сообщая.
$inf = Join-Path $pkg "bomboec_cable.inf"
$now = (Get-Date).ToUniversalTime()  # inf2cat сверяет дату с UTC: локальная дата бывает "из будущего"
$inv = [Globalization.CultureInfo]::InvariantCulture
$version = "1.0.{0}.{1}" -f ($now - [datetime]::new(2026, 1, 1)).Days, ($now.Hour * 60 + $now.Minute)
$driverVer = "DriverVer={0},{1}" -f $now.ToString("MM/dd/yyyy", $inv), $version
$text = [IO.File]::ReadAllText($inf) -replace '(?m)^DriverVer\s*=.*$', $driverVer
[IO.File]::WriteAllText($inf, $text, [Text.UTF8Encoding]::new($false))
Write-Host $driverVer

# Каталог подписи пакета.
$inf2cat = Join-Path $WdkBin "..\x86\inf2cat.exe"
if (-not (Test-Path $inf2cat)) { $inf2cat = Join-Path $WdkBin "inf2cat.exe" }
& $inf2cat /driver:"$pkg" /os:10_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed" }

# Тестовый сертификат (один раз) в личном хранилище пользователя.
$subject = "CN=bomboec test signing"
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath (Join-Path $pkg "bomboec_test.cer") -Force | Out-Null

$signtool = Join-Path $WdkBin "signtool.exe"
foreach ($f in @("bomboec_cable.sys", "bomboec_cable.cat")) {
    & $signtool sign /v /fd sha256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com (Join-Path $pkg $f)
    if ($LASTEXITCODE -ne 0) {
        # без сети: подпись без timestamp
        & $signtool sign /v /fd sha256 /sha1 $cert.Thumbprint (Join-Path $pkg $f)
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f" }
    }
}
Write-Host "package ready: $pkg"
Get-ChildItem $pkg
