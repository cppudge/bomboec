# Запись корпуса эталонов для регрессии (tests/corpus/corpus.toml): семь сценариев подряд,
# каждый через bomboec-rec (микрофон raw + loopback колонок), музыка через bomboec-play.
#   pwsh scripts/record-corpus.ps1                      # устройства из bomboec.toml / bomboec.state.toml
#   pwsh scripts/record-corpus.ps1 -Mic "<id>" -Speakers "<id>" -Music song.wav -Only music,speech
# Без -Music в колонки идёт полосовой шум 100-6000 Hz (bomboec-play --noise). Перед каждым
# сценарием скрипт говорит, что делать, и ждёт Enter. Результат: recordings/corpus/<name>/.
param(
    [string]$Mic = "",
    [string]$Speakers = "",
    [string]$Music = "",
    [double]$GainDb = -14,
    [string[]]$Only = @(),
    [string]$Bin = "build/ninja-release/bin"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$rec = Join-Path $Bin "bomboec-rec.exe"
$play = Join-Path $Bin "bomboec-play.exe"
if (-not (Test-Path $rec)) { throw "$rec not found: build first" }

# Устройства движка, если явно не заданы: bomboec.state.toml (выбор в меню) перекрывает
# [devices] из bomboec.toml. Микрофон по умолчанию Windows не годится: после установки
# драйвера это микрофон кабеля, который без bomboec.exe молчит.
function Read-DeviceId([string]$file, [string]$key) {
    if (-not (Test-Path $file)) { return "" }
    $m = Select-String -Path $file -Pattern "^$key\s*=\s*['`"](.+)['`"]" | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return ""
}
foreach ($file in @((Join-Path $Bin "bomboec.toml"), (Join-Path $Bin "bomboec.state.toml"))) {
    $id = Read-DeviceId $file "mic"
    if ($id) { $Mic = $id }
    $id = Read-DeviceId $file "speakers"
    if ($id) { $Speakers = $id }
}
if ($PSBoundParameters.ContainsKey("Mic")) { $Mic = $PSBoundParameters["Mic"] }
if ($PSBoundParameters.ContainsKey("Speakers")) { $Speakers = $PSBoundParameters["Speakers"] }
$devices = & $rec --list
$micLine = if ($Mic) { ($devices | Select-String -Pattern ([regex]::Escape($Mic)) -Context 1,0).Context.PreContext } else { "system default" }
$spkLine = if ($Speakers) { ($devices | Select-String -Pattern ([regex]::Escape($Speakers)) -Context 1,0).Context.PreContext } else { "system default" }
Write-Host "microphone: $($micLine -join '')"
Write-Host "speakers:   $($spkLine -join '')"
if (-not $Mic -or "$micLine" -match "bomboec Cable") {
    throw "choose the physical microphone: pass -Mic <id> (ids: $rec --list) or select it in the bomboec tray menu"
}
if (Get-Process -Name bomboec -ErrorAction SilentlyContinue) {
    Write-Host "bomboec.exe is running: it holds the microphone in raw mode, stopping it for the recording"
    Stop-Process -Name bomboec -Force
    Start-Sleep -Seconds 1
}

$scenarios = @(
    @{ name = "silence";      seconds = 20; music = $false; text = "Тишина: не говорите, не печатайте, музыки нет." },
    @{ name = "noise";        seconds = 20; music = $false; text = "Бытовой шум без речи: печатайте, двигайте мышь, пусть шумит вентилятор." },
    @{ name = "music";        seconds = 30; music = $true;  text = "Музыка в колонках, вы молчите." },
    @{ name = "speech";       seconds = 30; music = $false; text = "Только речь: читайте любой текст вслух все 30 секунд." },
    @{ name = "music-speech"; seconds = 30; music = $true;  text = "Музыка в колонках и вы читаете текст вслух все 30 секунд." },
    @{ name = "transients";   seconds = 20; music = $false; text = "Стуки без речи и музыки: стучите по столу, щёлкайте ручкой, ставьте кружку, задевайте стойку микрофона, с паузами 1-2 с (нужно не меньше 10 ударов)." },
    @{ name = "speech-call";  seconds = 30; music = $false; text = "Речь как в звонке: говорите живо и громко, как с собеседником, смейтесь, восклицайте, 30 секунд. Без музыки." }
)
foreach ($s in $scenarios) {
    if ($Only.Count -gt 0 -and $Only -notcontains $s.name) { continue }
    $out = Join-Path "recordings/corpus" $s.name
    Write-Host ""
    Write-Host "=== $($s.name) ($($s.seconds) s): $($s.text)"
    Read-Host "Enter, когда готовы"
    $player = $null
    if ($s.music) {
        $playArgs = @("--output", $Speakers, "--seconds", ($s.seconds + 3), "--gain-db", $GainDb)
        if ($Music) { $playArgs += @("--wav", $Music) } else { $playArgs += "--noise" }
        $player = Start-Process -FilePath $play -ArgumentList $playArgs -PassThru -NoNewWindow
        Start-Sleep -Seconds 1
    }
    $recArgs = @("--out", $out, "--seconds", $s.seconds)
    if ($Mic) { $recArgs += @("--mic", $Mic) }
    if ($Speakers) { $recArgs += @("--speakers", $Speakers) }
    & $rec @recArgs
    if ($player) { $player.WaitForExit() }
}
Write-Host ""
Write-Host "done. Run: $Bin\bomboec_tests.exe `"[corpus]`" (BOMBOEC_CORPUS_BASELINE=1 prints baseline lines for tests/corpus/corpus.toml)"
