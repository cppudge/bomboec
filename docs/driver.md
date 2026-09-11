# Драйвер виртуального кабеля bomboec Cable

`driver/` содержит kernel-mode драйвер `bomboec_cable.sys`: WaveRT/portcls на базе Microsoft
SimpleAudioSample (MIT). Render endpoint "Speakers (bomboec Cable)" и capture endpoint
"Microphone (bomboec Cable)" соединены кольцевым буфером в ядре (`driver/src/cable.cpp`): то, что
приложение играет в Speakers, любое другое приложение слышит из Microphone. Формат обоих
endpoint'ов 48 kHz, 2 ch, 16-bit PCM. Генератор тона и запись в файлы из образца удалены.

## Как устроен кабель

- Обе стороны ведут позиции от одного QPC с шагом DPC 1 ms, поэтому относительного дрейфа между
  ними нет, и prime (задержка кабеля, 10 ms) нужен только на джиттер DPC.
- При старте чтения (приложение открыло Microphone кабеля) всё, что render-сторона накопила сверх
  prime, отбрасывается: иначе backlog (до 1 с ёмкости буфера) остался бы в задержке навсегда.
- WaveRT сдвигает позиции на произвольное число байт, поэтому граница кадра (4 байта)
  отслеживается по фазе позиции потока; при старте чтения хвост подгоняется под фазу capture.
- При опустошении capture получает тишину и снова ждёт накопления prime; переполнение отбрасывает
  самые старые данные.
- `GetReadPacket` возвращает время первого сэмпла пакета, а не его конца, как в образце.

Синхронизация: spinlock кольца на DISPATCH_LEVEL, порядок захвата `m_PositionSpinLock` ->
`g_Cable.m_lock` без циклов (проверено ревью в сентябре 2026).

## Сборка и установка

Сборка не требует расширения WDK для Visual Studio: `driver/CMakeLists.txt` вызывает cl/link с
kernel-флагами напрямую (WDK 10.0.26100, KMDF 1.33, MSVC из VS 18).

```powershell
pwsh ./driver/build.ps1        # build/driver/package: .sys, .inf, .cat, тестовый .cer
# из PowerShell от администратора, режим тестовой подписи должен быть включён:
pwsh ./driver/install.ps1      # доверие тестовому сертификату + devcon install ROOT\BomboecCable
pwsh ./driver/uninstall.ps1
```

`build.ps1` создаёт тестовый сертификат в личном хранилище пользователя (один раз) и подписывает
пакет, при наличии сети с timestamp. Для чужих машин test-signing не годится: нужна attestation-
подпись через Microsoft Partner Center, до неё выходом служит VB-Cable (выбирается в меню как
любой render endpoint).

После установки в bomboec.exe выбирается Output = "Speakers (bomboec Cable)", а в Discord и прочих
приложениях микрофон = "Microphone (bomboec Cable)". Windows при установке делает кабель
устройством по умолчанию для вывода и ввода: defaults нужно вернуть на реальные устройства
(движок сам откажется брать микрофон кабеля при выходе в тот же кабель). Идентификаторы
endpoint'ов кабеля меняются при каждой переустановке драйвера.

## Счётчики кабеля

Кольцо считает опустошения (capture читал раньше, чем render записал), переполнения,
выравнивания фазы кадра и байты, отброшенные при прайме. При изменении, не чаще раза в секунду,
драйвер печатает их через `DbgPrintEx` (компонент IHVAUDIO, уровень WARNING):

```text
bomboec_cable: underruns 0, overruns 0, realigns 1, trimmed 38400 bytes
```

Чтобы видеть их в DebugView (Capture Kernel) или WinDbg, один раз включите фильтр и
перезагрузитесь:

```powershell
# от администратора
$key = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter"
if (-not (Test-Path $key)) { New-Item $key | Out-Null }
New-ItemProperty -Path $key -Name IHVAUDIO -PropertyType DWord -Value 8 -Force
```

Проверка запаса prime: час `bomboec-play` в Speakers кабеля и `bomboec-rec` с Microphone кабеля.
Если опустошения растут (capture и render DPC попадают в один тик в разном порядке), prime
увеличивается на 2-3 ms (`CABLE_PRIME_BYTES` в `driver/src/adapter.cpp`, сейчас ровно период
10 ms).

Для периодической проверки на тестовой машине: Driver Verifier
(`verifier /standard /driver bomboec_cable.sys`) и `/analyze` из WDK.

## История

До правки прайма кабель, у которого Speakers писались весь день, а Microphone открыли позже,
отдавал звук с задержкой около 1 с (весь накопленный буфер). Замеченное ранее "отставание меток
loopback на 490 ms" было тем же backlog'ом, а не ошибкой меток; версия про дрейф независимых
таймеров не подтвердилась (capture-сторона показывает 0 ppm к QPC).
