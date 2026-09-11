# Запись корпуса эталонов для регрессии (tests/corpus/corpus.toml): пять сценариев подряд,
# каждый через bomboec-rec (микрофон raw + loopback колонок), музыка через bomboec-play.
#   pwsh scripts/record-corpus.ps1                      # устройства из bomboec.state.toml / по умолчанию
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

# Устройства, выбранные в меню трея, если явно не заданы.
$state = Join-Path $Bin "bomboec.state.toml"
if (-not $Mic -and (Test-Path $state)) {
    $m = Select-String -Path $state -Pattern '^mic\s*=\s*[''"](.+)[''"]' | Select-Object -First 1
    if ($m) { $Mic = $m.Matches[0].Groups[1].Value }
}
if (-not $Speakers -and (Test-Path $state)) {
    $m = Select-String -Path $state -Pattern '^speakers\s*=\s*[''"](.+)[''"]' | Select-Object -First 1
    if ($m) { $Speakers = $m.Matches[0].Groups[1].Value }
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
    @{ name = "music-speech"; seconds = 30; music = $true;  text = "Музыка в колонках и вы читаете текст вслух все 30 секунд." }
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
