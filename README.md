# bomboec

Лёгкое Windows-приложение: убирает из сигнала физического микрофона звук, который играют
колонки (AEC), при необходимости давит шум (NS), и отдаёт результат в виртуальный микрофон
для Discord, Zoom, браузера, OBS.

- Системный звук не трогаем: reference для AEC берётся через WASAPI loopback с выбранных
  колонок, воспроизведение идёт в штатном формате.
- AEC: WebRTC AEC3 из `webrtc-audio-processing` 2.1 (freedesktop); NS: RNNoise 0.2 (xiph).
  Обработка собрана из стадий, цепочка и их параметры задаются конфигом.
- Выход: свой драйвер виртуального кабеля bomboec Cable (`driver/`) или любой другой render
  endpoint, например VB-Cable.
- DSP: 48 kHz, float32, кадр 10 ms. Задержка микрофон -> выход около 55 ms.

Документация:

| Файл | О чём |
|---|---|
| [docs/architecture.md](docs/architecture.md) | модули, потоки и кольца, выравнивание reference, бюджет задержки, устойчивость |
| [docs/driver.md](docs/driver.md) | драйвер кабеля: устройство, сборка, установка, счётчики |
| [docs/measurements.md](docs/measurements.md) | замеры на реальных устройствах и в симуляции, с датами |
| [docs/decisions.md](docs/decisions.md) | принятые решения и их причины |
| [docs/research/](docs/research/windows-aec-application-design.md) | исходное исследование и обоснование подхода |

## Окружение

- Windows 10 1903+ / 11 x64, Visual Studio 2022/2026 с C++ (MSVC 19.4x+), Windows SDK 10.0.22621+.
- CMake 3.28+, Ninja, Conan 2.x в PATH. Meson и pkgconf для сборки рецептов Conan подтягивает сам.
- Для драйвера: WDK 10.0.26100 (см. [docs/driver.md](docs/driver.md)).

## Сборка

```powershell
./build.ps1                  # Release: configure + build + ctest
./build.ps1 -Preset debug    # Debug (-Debug то же самое)
./build.ps1 -Preset asan     # Debug + AddressSanitizer
./build.ps1 -Clean           # чистая сборка пресета
```

Скрипт поднимает окружение MSVC (vcvars64 через vswhere, если `cl.exe` нет в PATH) и вызывает
`cmake --workflow --preset <preset>`. Из Developer PowerShell, VS Code (CMake Tools) или Visual
Studio (Open Folder) пресеты работают напрямую: `cmake --preset release`, `cmake --build --preset
release`, `ctest --preset release`. Бинарники и их PDB лежат в `build/ninja-<preset>/bin/`.

`conan install` запускает сам CMake (cmake-conan provider, `cmake/cmake-conan/`). Conan работает в
домашней папке проекта `.conan2/` (conancenter и локальный индекс рецептов `conan-recipes/`),
глобальные профили и remotes пользователя на сборку не влияют. Версии и ревизии зависимостей
зафиксированы в `conan.lock`; после изменения `conanfile.py` или рецептов он обновляется так:

```powershell
$env:CONAN_HOME = "$PWD/.conan2"
$p = "build/ninja-release/conan_host_profile"
conan lock create . -pr:h $p -pr:b $p -s build_type=Release --lockfile-out=conan.lock
conan lock create . -pr:h $p -pr:b $p -s build_type=Debug --lockfile=conan.lock --lockfile-out=conan.lock
```

CRT линкуется статически: exe не требуют VC++ Redistributable.

## Запуск

`bomboec.exe` живёт в трее. Каталог данных: рядом с exe, если там лежит `bomboec.toml` или пустой
файл-маркер `portable` (сборка разработчика, флешка), иначе `%LOCALAPPDATA%omboec` (под
Program Files рядом с exe писать нельзя). Путь виден в окне статуса. В каталоге:

| Файл | Кто пишет | Что это |
|---|---|---|
| `bomboec.toml` | пользователь | конфиг; при первом запуске создаётся из встроенного `config/default.toml` |
| `bomboec.state.toml` | приложение | устройства, выбранные в меню; его `[devices]` перекрывает `[devices]` конфига |
| `bomboec.log`, `.log.1` | приложение | лог, ротация на 1 MB; раз в минуту строка `status:` со счётчиками движка |
| `bomboec-*.dmp` | приложение | минидамп при падении (разбирается с `bomboec.pdb` той же сборки) |

Меню: старт/стоп, выбор микрофона, колонок (reference) и выхода, окно статуса, открыть конфиг и
лог, перечитать конфиг. Левый клик по иконке открывает окно статуса: уровни, delay/ERL/ERLE,
пропуски и скачки reference, буфер выхода, дрейф часов, версия.

Выход в те же колонки, что служат reference, и микрофон, который является другим концом
выходного кабеля, движок не открывает: это петли. Если микрофон или выход пропали или микрофон
перестал отдавать звук, движок перезапускается с паузами 1, 2, 5, 10, 30 с, пока устройство не
вернётся. Колонки (reference) необязательны: без них микрофон идёт в выход без подавления эха,
в статусе висит предупреждение, и раз в 5 с движок пробует открыть loopback снова.

Микрофон по требованию (`on_demand = true`): физический микрофон занят, только пока какое-то
приложение пишет с микрофона кабеля (Discord в звонке, OBS). Раз в секунду приложение смотрит
аудиосессии на другом конце выхода; если слушателей нет `idle_stop_sec` (5) подряд, движок
останавливается (иконка «idle», микрофон свободен, индикатор микрофона Windows гаснет), а с
первой сессией стартует снова: первая секунда звонка идёт в тишине, AEC3 сходится ещё пару
секунд. Если выход не виртуальный кабель или сессии узнать нельзя, микрофон держится постоянно.

### Конфиг

`config/default.toml` описывает все ключи с комментариями. Значения проверяются по типу и
диапазону; неизвестный ключ (опечатка) даёт предупреждение в логе и в уведомлении.

`[engine]`: `mic_raw` (true, raw mode микрофона без системных APO), `reference_lead_ms` (5),
`output_buffer_ms` (10, запас в выходном кольце), `output_render_ms` (20, заполнение буфера WASAPI
выхода), `output_channels` (2), `record_dir` (пусто; иначе debug-запись mic_raw/ref/out в WAV),
`on_demand` (true, микрофон по требованию), `idle_stop_sec` (5, 1..3600).

`reference_lead_ms`: reference для кадра микрофона берётся на столько раньше времени кадра. AEC3
ищет эхо только в прошлом reference, поэтому задержка динамик -> микрофон (на машине разработки
около 32 ms по оценке `aec delay` в окне статуса, на USB-ЦАП бывает 10-15 ms) должна быть больше
lead с запасом; иначе эхо не подавляется, а `delay` в статусе показывает 0-4 ms. Больше 5 ms не
нужно: loopback отдаёт данные раньше их метки (engine смешивает на период вперёд), и `missing` в
статусе не растёт даже при lead 0.

Цепочка `[[chain]]` выполняется по порядку; каждая возможность (hpf, aec, ns, agc, limiter, transient) может
быть объявлена только одной стадией. Ключи стадий проверяются по типу и диапазону (`StageParams`),
неизвестный ключ даёт предупреждение:

| id | Возможности | Ключи |
|---|---|---|
| `hpf` | hpf | `cutoff_hz` (80) |
| `webrtc` | aec, и по флагам hpf/ns/agc | `aec` (true), `hpf` (false), `ns` (false; в шаблоне true), `ns_level` (moderate; в шаблоне high: low, moderate, high, very_high), `agc` (false), `filter_length_blocks` (13, 1..60), `delay_num_filters` (5, 1..20) |
| `transient` | transient | `rise_db_per_ms` (20, 5..60), `level_dbfs` (-30, -80..0), `depth_db` (20, 0..60), `hold_ms` (60, 0..1000), `release_ms` (100, 1..5000) |
| `rnnoise` | ns | без ключей |
| `limiter` | limiter | `ceiling_db` (-1.0, -60..0), `release_ms` (50, 0.1..10000) |

Шумоподавление: в шаблоне стадия `rnnoise` (рекуррентная сеть xiph, задержка один кадр);
NS из WebRTC остаётся в стадии `webrtc` (`ns = true`, тогда `rnnoise` из цепочки убрать: две стадии
с одной возможностью не допускаются). На корпусе записей (docs/measurements.md) RNNoise давит
клавиатуру на 18 dB против 5 dB у WebRTC при той же потере речи. Громкие удары с резкой атакой
(стук по столу, ручка, кружка) RNNoise принимает за речь и пропускает: их гасит стадия `transient`
перед ним, гейт по скорости нарастания огибающей за 1 ms (речь не быстрее 15-20 dB/ms, удары
25-45) с удержанием на отскоки. После правки конфига пункт меню «Reload config».

Интерфейс стадии и правила для новых бэкендов: `src/core/stage.h` (новая стадия регистрируется в
`StageRegistry` по строковому id).

## Утилиты

Все печатают `--help`.

```powershell
# список устройств и синхронная запись микрофона + loopback (mic.wav, ref.wav, metadata.json)
bomboec-rec --list
bomboec-rec --out recordings/take1 --seconds 20 --mic "<id>" --speakers "<id>"

# офлайн-прогон записанной пары через цепочку: подавление, статистика AEC3, CSV
bomboec-proc --mic take/mic.wav --ref take/ref.wav --config config/default.toml --out take/out.wav --csv take/stats.csv

# движок в консоли, с debug-записью того, что он реально видел
bomboec-run --config config/default.toml --seconds 30 --mic "<id>" --speakers "<id>" --output "<id>" --record recordings/live1

# чирп, полосовой шум или WAV в render endpoint (проверка кабеля, калибровка задержки, запись корпуса)
bomboec-play --output "<id>" --seconds 10 [--noise | --wav music.wav]

# кто держит аудиопотоки открытыми (процессы с активной сессией на каждом endpoint'е);
# код возврата 1, если такие есть: install.ps1 так находит, кто мешает обновить драйвер
bomboec-sessions [--filter "bomboec Cable"] [--all]
```

`bomboec-proc` в итоге печатает подавление на кадрах с активным reference, подавление без
reference (мера искажения речи, около 0 dB без учёта HPF) и момент, когда полное подавление за
секунду впервые достигло 10 dB. Протокол записи для оценки AEC: 30 секунд, первые 10 только музыка
из колонок, следующие 10 музыка плюс речь в микрофон (double-talk), последние 10 только речь.

## Разработка

- Стиль: `.clang-format` (эталон clang-format 22, LLVM в `C:\Program Files\LLVM\bin`), драйвер не
  форматируется. Проверка перед коммитом: `git config core.hooksPath .githooks`.
- Статический анализ: `.clang-tidy` (bugprone, clang-analyzer, performance, concurrency, именование,
  несколько modernize; отключённые проверки объяснены в файле). Те же проверки показывает clangd.
- `pwsh scripts/check.ps1` (clang-tidy + clang-format, `-Fix` применяет исправления).
- `pwsh scripts/ci.ps1`: сборка и тесты пресетов release, debug, asan, затем `check.ps1`.
- Тесты (Catch2, `tests/`): модули ядра, поддельные WASAPI-объекты для путей ошибок устройств
  (`tests/fakes/`), детерминированный симулятор часов и пакетов для конвейера (`tests/sim/`),
  подавление эха AEC3 при джиттере и дрейфе. Скрытый бенчмарк: `bomboec_tests "[aec-benchmark]"`.
- Регрессия на эталонных записях (`[corpus]`): пары mic.wav + ref.wav из `bomboec-rec`
  прогоняются через настоящий Pipeline с измеренным дрейфом и цепочкой из шаблона, по сегментам
  (эхо, double-talk, речь, тишина, шум, стуки) проверяются подавление, сохранность речи, всплески и
  ухудшение относительно базовых значений. Манифест `tests/corpus/corpus.toml`, записи в
  `recordings/` (вне git; без них тест пропускается). Записать корпус:
  `pwsh scripts/record-corpus.ps1` (шесть сценариев с подсказками, музыка через `bomboec-play
  --noise` или `-Music file.wav`); обновить базовые значения: `BOMBOEC_CORPUS_BASELINE=1
  bomboec_tests "[corpus]"`.

## Структура

```text
conanfile.py, conan.lock      зависимости и их зафиксированные версии
CMakeLists.txt                корневой проект, cmake-conan, флаги (bomboec::options)
CMakePresets.json             пресеты release, debug, asan (configure, build, test, workflow)
build.ps1                     окружение MSVC + cmake --workflow
cmake/                        cmake-conan provider, манифест exe (UTF-8), генерация version.h
scripts/                      vcvars.ps1, check.ps1 (clang-tidy + clang-format), ci.ps1, record-corpus.ps1
conan-recipes/recipes/        локальные рецепты (webrtc-audio-processing, rnnoise)
config/default.toml           конфигурация по умолчанию (встраивается в bomboec.exe)
docs/                         архитектура, драйвер, замеры, решения, исследование
src/core/                     Frame, RingBuffer, Timeline, PacketAssembler, FillController, SeqLock, WAV, IStage, StageParams, Chain, конфиг
src/stages/                   hpf, webrtc (AEC3 + hpf/ns/agc из APM), rnnoise, limiter
src/wasapi/                   устройства, CaptureStream (mic/loopback), RenderStream (keepalive, выход)
src/engine/                   Pipeline (DSP без устройств), Engine (Pipeline + WASAPI), Watchdog, Recorder
src/app/                      Controller (политика без Win32, тестируется), bomboec.exe (трей, окно статуса), каталог данных, минидампы
src/tools/                    bomboec-rec, bomboec-proc, bomboec-run, bomboec-play, apm_smoke
driver/                       драйвер кабеля bomboec_cable.sys
tests/                        Catch2: модули, fakes/ (WASAPI), sim/ (симулятор конвейера), corpus/ (манифест эталонов)
```

## Этапы

| Этап | Результат |
|---|---|
| 0. Бутстрап (готово) | conanfile, рецепт AEC3, скелет CMake, скрипт сборки |
| 1. Ядро (готово) | Frame, ring, timeline, IStage, Chain, Registry, конфиг TOML, тесты |
| 2. Рекордер (готово) | WASAPI mic в raw mode, loopback с keepalive, QPC-метки, WAV + metadata.json |
| 3. Офлайн-процессор (готово) | WAV через цепочку, подавление и статистика APM |
| 4a. Realtime (готово) | движок на mic-потоке, reference по таймлайну, трей с диагностикой |
| 4b. Virtual cable (готово) | драйвер на базе SimpleAudioSample, сборка через CMake, test-signing |
| 4c. Задержка (готово) | обрезка backlog'а кабеля, prime 10 ms, целевое заполнение выхода, регулятор |
| 5. Устойчивость (частично) | сделано: watchdog с backoff, ресинхронизация таймлайнов, непрерывный reference, предел задержки, защита от петель, минидампы, reference необязателен, уведомления об устройствах (IMMNotificationClient), каталог данных в %LOCALAPPDATA% с портативным режимом, строка статуса в лог раз в минуту. Осталось: суточный прогон, малые периоды IAudioClient3, адаптивный ресемплинг reference |
| 6. Второй бэкенд (частично) | сделано: рецепт и стадия rnnoise, сравнение на корпусе. Осталось: speexdsp как дешёвый второй AEC, опционально DLL-плагины |
| 7. Дистрибуция | установщик, attestation-подпись драйвера (или VB-Cable), автозапуск |

Бэкенды на будущее: AEC — SpeexDSP (дешёвый второй), LocalVQE (нейросетевой, 16 kHz); NS —
DeepFilterNet3.
