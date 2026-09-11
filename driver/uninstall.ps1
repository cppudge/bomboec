# Удаление bomboec_cable (права администратора).
param(
    [string]$WdkTools = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64"
)
$ErrorActionPreference = "Stop"
$devcon = Join-Path $WdkTools "devcon.exe"
& $devcon remove "ROOT\BomboecCable"
# Убираем пакет из DriverStore.
$published = pnputil /enum-drivers | Select-String -Context 0,6 "bomboec_cable.inf"
foreach ($m in $published) {
    $line = ($m.Context.PreContext + $m.Line + $m.Context.PostContext) | Select-String "Published Name" | Select-Object -First 1
    if (-not $line) { $line = $m.Line }
    if ($line -match "(oem\d+\.inf)") { pnputil /delete-driver $Matches[1] /uninstall /force }
}
Write-Host "removed"
