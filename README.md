# bomboec

Лёгкое Windows-приложение: убирает из сигнала физического микрофона звук, который играют
колонки (AEC), при необходимости давит шум (NS), и отдаёт результат в виртуальный микрофон
для Discord, Zoom, браузера, OBS.

Ключевые решения:

- Системный звук не трогаем. Reference для AEC берётся через WASAPI loopback с выбранного
  физического render-устройства, воспроизведение идёт в штатном формате.
- Внутренний формат DSP: 48 kHz, float32 planar, кадр 10 ms (480 сэмплов).
- Основной AEC-бэкенд: WebRTC AEC3 через `webrtc-audio-processing` 2.x (freedesktop).
  Бэкенды подключаются как стадии цепочки и заменяются через конфиг.
- Виртуальный микрофон в MVP: сторонний virtual cable (VB-Cable), в который пишем
  обычным WASAPI render. Свой драйвер только при необходимости.
- Зависимости: Conan 2, локальный индекс рецептов для библиотек, которых нет в Conan Center.

Подробное исследование и обоснование: `windows-aec-application-design.md`.
Примечание: NVIDIA Maxine AEC из того документа снят с поддержки в AFX SDK 3.0 и в план не входит.

## Окружение

- Windows 10/11 x64, Visual Studio 2022/2026 с C++ (MSVC 19.4x+), Windows SDK 10.0.22621+.
- CMake 3.28+, Ninja, Conan 2.x в PATH. Meson и pkgconf для сборки рецептов Conan подтягивает сам.

## Сборка

```powershell
./build.ps1            # Release: configure + build + ctest; -Debug для Debug, -Clean для чистой сборки
```

Скрипт поднимает окружение MSVC (vcvars64 через vswhere, если `cl.exe` нет в PATH) и вызывает
`cmake --workflow --preset release`. Из Developer PowerShell, VS Code (CMake Tools) или
Visual Studio (Open Folder) пресеты работают напрямую:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Бинарники (bomboec.exe, утилиты, тесты) лежат в `build/ninja-release/bin/`.

`conan install` запускает сам CMake при настройке (cmake-conan provider, `cmake/cmake-conan/`).
Conan работает в домашней папке проекта `.conan2/`: в ней только conancenter и локальный индекс
рецептов `conan-recipes/` (remote `bomboec-local`), поэтому глобальные профили и remotes
пользователя на сборку не влияют. Настройки (компилятор, cppstd, runtime, build type) Conan
получает из того, что нашёл CMake. Первая настройка собирает зависимости в этот кэш
(webrtc-audio-processing и abseil из исходников, несколько минут). Ручные команды conan с тем же
кэшем: `$env:CONAN_HOME = "$PWD/.conan2"`. `-DBOMBOEC_LOCAL_CONAN_HOME=OFF` берёт глобальную
домашнюю папку Conan, remote `bomboec-local` тогда нужно добавить в неё самому.

Рецепт `webrtc-audio-processing` пинит abseil 20240722 (более новые abseil убрали
`absl::Nullable`) и добавляет в библиотеку `api/audio/echo_canceller3_factory.h`, которой нет
в тарболе freedesktop: без неё нельзя передать `EchoCanceller3Config` через публичный API.

## Стиль кода

Форматирование задаёт `.clang-format` (эталон clang-format 22, LLVM ставится в
`C:\Program Files\LLVM\bin`). Драйвер не форматируется (`driver/.clang-format`): он сохраняет
стиль образца Microsoft. Проверка форматирования перед коммитом включается один раз:

```powershell
git config core.hooksPath .githooks
```

## Структура

```text
conanfile.py                  зависимости приложения
CMakeLists.txt                корневой проект, подключение cmake-conan
CMakePresets.json             пресеты release/debug (configure, build, test, workflow)
build.ps1                     окружение MSVC + cmake --workflow
cmake/cmake-conan/            cmake-conan provider (conan install из CMake)
scripts/                      вспомогательные скрипты (vcvars.ps1)
conan-recipes/recipes/        локальные рецепты (webrtc-audio-processing, позже speexdsp, rnnoise)
config/default.toml           конфигурация конвейера по умолчанию
src/core/                     Frame, RingBuffer, Timeline, PacketAssembler, WAV, IStage, Chain, StageRegistry, config
src/stages/                   hpf, webrtc (AEC3 + hpf/ns/agc из APM), limiter; позже speex_aec, rnnoise, ...
src/wasapi/                   devices, CaptureStream (mic/loopback), RenderStream (keepalive, позже cable)
src/tools/                    apm_smoke, bomboec-rec, bomboec-proc, bomboec-run (движок в консоли), bomboec-play (тестовый сигнал)
src/engine/                   Engine: realtime-конвейер mic -> reference по таймлайну -> Chain -> render
src/app/                      bomboec.exe, tray-приложение вокруг Engine
tests/                        Catch2
```

### Стадии и их ключи конфига

| id | Caps | Ключи |
|---|---|---|
| `hpf` | hpf | `cutoff_hz` (80) |
| `webrtc` | aec, и по флагам hpf/ns/agc | `aec` (true), `hpf` (false), `ns` (false), `ns_level` (moderate), `agc` (false), `filter_length_blocks` (13), `delay_num_filters` (5) |
| `limiter` | limiter | `ceiling_db` (-1.0), `release_ms` (50) |

## Интерфейс стадии

```cpp
enum class Cap : uint32_t { Hpf = 1, Aec = 2, Ns = 4, Agc = 8 };

struct StageInfo {
    std::string id;
    uint32_t sampleRate;      // 48000 для всех текущих стадий
    uint32_t frameSamples;    // 480
    uint32_t caps;            // битовая маска Cap
    uint32_t latencyFrames;
};

class IStage {
public:
    virtual ~IStage() = default;
    virtual StageInfo info() const = 0;
    virtual bool init(const PipelineFormat& fmt, const toml::table& cfg) = 0;
    // mic обрабатывается in-place; reference уже выровнен движком,
    // nullptr для стадий без Cap::Aec
    virtual void process(Frame& mic, const Frame* reference) = 0;
    virtual void reset() = 0;
    virtual StageStats stats() const = 0;
};
```

Замена бэкенда: новый класс плюс регистрация в `StageRegistry` по строковому id из конфига.
Цепочка проверяет, что ни одна возможность (Cap) не объявлена дважды. В MVP стадии линкуются
статически, загрузка из DLL добавляется позже без смены интерфейса.

Стадия WebRTC: `AudioProcessingBuilder` с `EchoCanceller3Factory` и явным `EchoCanceller3Config`;
на каждый кадр `ProcessReverseStream` со стерео reference, затем `ProcessStream` с mono mic.
Статистика для диагностики из `GetStatistics`. NS и AGC внутри APM включаются флагами конфига.

## Рекордер

```powershell
./build/ninja-release/bin/bomboec-rec.exe --list
./build/ninja-release/bin/bomboec-rec.exe --out recordings/take1 --seconds 20 --mic "<id>" --speakers "<id>"
```

Пишет `mic.wav` (mono) и `ref.wav` (stereo), 48 kHz float32, выровненные по QPC-меткам
пакетов: сэмпл N обоих файлов соответствует одному моменту времени с точностью до дрейфа.
`metadata.json` содержит устройства, пропуски/наложения, джиттер меток и оценку дрейфа.
Микрофон открывается в raw mode (если endpoint позволяет), loopback держится живым
собственным потоком тишины (`--no-keepalive` показывает, что без него пакетов нет вовсе).

Замеры на этой машине (сентябрь 2026): джиттер меток до 0.2 ms у Yeti и 0.03 ms у loopback,
относительный дрейф микрофон/колонки около 140 ppm. Это аргумент в пользу адаптивного
ресемплинга reference на этапе 5. Виртуальный микрофон NVIDIA Broadcast (default) не
принимает raw mode и сам обрабатывает звук, для записей нужен физический микрофон.

## Офлайн-процессор

```powershell
./build/ninja-release/bin/bomboec-proc.exe --mic take/mic.wav --ref take/ref.wav --config config/default.toml --out take/out.wav --csv take/stats.csv
```

Гонит пару WAV через цепочку из конфига кадрами по 10 ms, пишет результат и раз в секунду
печатает уровни, подавление и статистику AEC3 (delay, ERL, ERLE). В итоге: подавление на кадрах
с активным reference, подавление на кадрах без reference (мера искажения речи, ожидается около 0 dB
без учёта HPF) и момент, когда ERLE впервые достиг 10 dB. `--ref-offset-ms` сдвигает reference
относительно mic для проверки статического выравнивания. CSV пригоден для построения графиков.

Протокол записи для оценки AEC: 30 секунд, первые 10 только музыка из колонок, следующие 10
музыка плюс речь в микрофон (double-talk), последние 10 только речь без музыки.

## Realtime-движок и tray-приложение

`bomboec.exe` живёт в трее, конфиг `bomboec.toml` рядом с exe создаётся из встроенного шаблона.
Меню: старт/стоп, выбор микрофона, колонок (reference) и выхода, окно статуса (уровни,
delay/ERL/ERLE, пропуски reference, буфер выхода, дрейф), открыть конфиг, перечитать конфиг.
Выбор устройства сохраняется в конфиг и перезапускает движок. При падении потока (устройство
пропало) движок перезапускается watchdog'ом раз в секунду.

Выход сейчас идёт в любой выбранный render endpoint. Пока нет драйвера virtual cable, для
проверки удобен любой существующий виртуальный render (например Steam Streaming Speakers).
Выход в те же колонки, что служат reference, движок отказывается открывать: это акустическая петля.

Консольный вариант для отладки:

```powershell
./build/ninja-release/bin/bomboec-run.exe --config config/default.toml --seconds 30 --mic "<id>" --speakers "<id>" --output "<id>" --record recordings/live1
```

`--record` включает debug-запись mic_raw/ref/out в WAV (то, что реально видел движок).

Выравнивание reference: для кадра микрофона со временем t (по QPC-таймлайну mic) reference
читается из кольцевого буфера по индексу `refTimeline.sampleAt(t - lead)`, lead = 20 ms как запас
на джиттер loopback. Дрейф часов компенсируется проскальзыванием на сэмпл, поэтому задержка,
которую видит AEC3, не растёт со временем. Плавный ресемплинг вместо проскальзывания: этап 5.

Задержка выхода (mic -> render endpoint) складывается из трёх управляемых частей:
- `output_buffer_ms` (10): целевой запас в выходном кольце после каждого чтения render-потоком,
  единственный резерв на джиттер микрофона. `FillController` (`core/fill_controller.h`) держит
  минимум остатка за окно 1 с около цели, растягивая или сжимая кадр на 1..4 сэмпла линейной
  интерполяцией (0.2..0.8 % высоты тона на 10 ms, без разрывов). Без него дрейф микрофона
  относительно выходного устройства (Yeti +23 ppm к QPC) копил бы около 80 ms в час.
- `output_render_ms` (20): до скольких ms дозаполняется буфер WASAPI выхода на каждое событие.
  Раньше буфер (60 ms) заполнялся до краёв, и всё это было задержкой. Меньше двух периодов
  engine (обычно 2 x 10 ms) ставить не стоит.
- сам период engine и буферы конечных устройств: в shared mode это по 10 ms на захват и на
  воспроизведение, сверх наших буферов.
В окне статуса строка `output` показывает текущее заполнение кольца, минимальный остаток
(margin) и цель WASAPI; `fill ctl` считает добавленные/убранные сэмплы. Если margin со временем
не уходит от цели, регулятор работает.

## Virtual cable (драйвер)

`driver/` содержит kernel-mode драйвер `bomboec_cable.sys`: WaveRT/portcls, на базе Microsoft
SimpleAudioSample (MIT). Render endpoint "Speakers (bomboec Cable)" и capture endpoint
"Microphone (bomboec Cable)" соединены кольцевым буфером в ядре (`src/cable.cpp`): то, что
приложение играет в Speakers, любое другое приложение слышит из Microphone. Формат обоих
endpoint'ов 48 kHz, 2 ch, 16-bit PCM, задержка кабеля 10 ms (prime). Генератор тона и запись
в файлы из образца удалены.

Обе стороны кабеля ведут позиции от одного QPC с шагом DPC 1 ms, поэтому относительного дрейфа
между ними нет и prime нужен только на задержку DPC. При старте чтения (приложение открыло
Microphone кабеля) всё, что render-сторона накопила сверх prime, отбрасывается: иначе backlog
(до 1 с ёмкости буфера) оставался бы в задержке навсегда. `GetReadPacket` возвращает время
первого сэмпла пакета, а не его конца, как в образце Microsoft.

Сборка не требует расширения WDK для Visual Studio: `driver/CMakeLists.txt` вызывает cl/link с
kernel-флагами напрямую (WDK 10.0.26100, KMDF 1.33, MSVC из VS 18).

```powershell
pwsh ./driver/build.ps1        # build/driver/package: .sys, .inf, .cat, тестовый .cer
# из PowerShell от администратора, режим тестовой подписи должен быть включён:
pwsh ./driver/install.ps1      # доверие тестовому сертификату + devcon install ROOT\BomboecCable
pwsh ./driver/uninstall.ps1
```

После установки в bomboec.exe выбирается Output = "Speakers (bomboec Cable)", а в Discord и
прочих приложениях микрофон = "Microphone (bomboec Cable)". Windows при установке делает кабель
устройством по умолчанию для вывода и ввода: defaults нужно вернуть на реальные устройства.
Идентификаторы endpoint'ов кабеля меняются при каждой переустановке драйвера.

Проверка кабеля насквозь: `bomboec-play` играет чирп в Speakers кабеля, `bomboec-rec` пишет
Microphone кабеля и loopback его Speakers, кросс-корреляция даёт задержку и точность.

История: до правки прайма кабель, у которого Speakers писались весь день, а Microphone
открыли позже, отдавал звук с задержкой около 1 с (весь накопленный буфер). Замеченное ранее
"отставание меток loopback на 490 ms" было тем же backlog'ом, а не ошибкой меток; версия про
дрейф независимых таймеров не подтвердилась (capture-сторона показывает 0 ppm к QPC).

## Принципы realtime-части

- Микрофон открывается в raw mode (`AUDCLNT_STREAMOPTIONS_RAW`), без категории Communications,
  с `AUTOCONVERTPCM` в 48 kHz float.
- Loopback молчит без активных render-потоков: держим свой тихий render-поток как keepalive и
  заполняем reference нулями по таймлайну QPC, когда пакетов нет.
- Mic-поток ведёт pipeline; отдельного AEC-потока нет. Потокам ставится MMCSS "Pro Audio".
- Reference выравнивается по QPC-меткам пакетов; AEC3 сам оценивает остаточную задержку.
  Статический сдвиг reference нужен только для устройств с большой задержкой (Bluetooth).
- Дрейф часов измеряется через `IAudioClock` (наклон position к QPC); адаптивный ресемплинг
  добавляется только если измерения покажут необходимость. Выходной ring в cable имеет
  третий клок и свой контроллер заполнения.
- Никаких аллокаций и блокировок в hot path.

## Этапы

| Этап | Результат | Критерий готовности |
|---|---|---|
| 0. Бутстрап (готово) | conanfile, рецепт AEC3, скелет CMake, скрипт сборки | тестовый бинарь линкуется с APM и прогоняет тишину |
| 1. Ядро (готово) | Frame, ring, timeline, IStage, Chain, Registry, конфиг TOML, тесты | стадии hpf и webrtc проходят unit-тесты на синтетике |
| 2. Рекордер (готово) | WASAPI mic в raw mode, loopback с keepalive, QPC-метки, multi-track WAV | синхронные записи с реального стола плюс metadata.json |
| 3. Офлайн-процессор | WAV в цепочку, WAV на выходе, ERLE и статистика APM | подобран конфиг AEC3 под конкретный setup |
| 4a. Realtime (готово) | движок на mic-потоке, reference по таймлайну, вывод в любой render endpoint, tray с диагностикой | движок работает вживую без пропусков reference |
| 4b. Virtual cable (готово) | драйвер-cable на базе SimpleAudioSample, сборка через CMake без VSIX, test-signing | чирп проходит через кабель с корреляцией 0.91, без пропусков пакетов |
| 4c. Задержка (готово) | обрезка backlog'а кабеля при прайме, prime 10 ms, целевое заполнение WASAPI выхода, регулятор заполнения выходного кольца | Yeti -> Speakers кабеля около 55 ms вместо 106, не растёт со временем |
| 5. Устойчивость | уведомления устройств, перезапуск, измерение дрейфа регрессией, статический сдвиг reference, малые периоды IAudioClient3 для кабеля | сутки работы без рассинхрона |
| 6. Второй бэкенд | рецепты speexdsp или rnnoise, стадии, опционально DLL-плагины | цепочка переключается через конфиг |
| 7. Драйвер | cable на SysVAD | только если VB-Cable перестанет устраивать |

## Бэкенды на будущее

AEC: WebRTC AEC3 (основной), SpeexDSP (дешёвый второй), LocalVQE (нейросетевой, 16 kHz, CPU).
NS: WebRTC NS, RNNoise, DeepFilterNet3, Maxine denoiser (GPU, через NGC).
