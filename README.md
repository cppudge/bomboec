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

- Windows 10/11 x64, Visual Studio 2022 (MSVC 19.4x), Windows SDK 10.0.22621+.
- CMake 3.28+, Ninja, Conan 2.x. Meson и pkgconf для сборки рецептов Conan подтягивает сам.

## Сборка

```powershell
.uild.ps1            # Release; .uild.ps1 -Debug для Debug, -Clean для чистой сборки
```

Скрипт делает то же, что и руками:

```powershell
conan remote add bomboec-local ./conan-recipes --type local-recipes-index   # один раз
conan install . -pr:a ./conan/profiles/msvc-release -r bomboec-local -r conancenter --build=missing
cmake --preset conan-release
cmake --build --preset conan-release
```

Профиль проекта лежит в `conan/profiles/` и не зависит от пользовательского default-профиля.
Remotes ограничены явно, чтобы недоступные корпоративные remotes не ломали разрешение графа.

Рецепт `webrtc-audio-processing` пинит abseil 20240722 (более новые abseil убрали
`absl::Nullable`) и добавляет в библиотеку `api/audio/echo_canceller3_factory.h`, которой нет
в тарболе freedesktop: без неё нельзя передать `EchoCanceller3Config` через публичный API.

## Структура

```text
conanfile.py                  зависимости приложения
CMakeLists.txt                корневой проект
build.ps1                     conan install + cmake + ctest
conan/profiles/               профили Conan для проекта
conan-recipes/recipes/        локальные рецепты (webrtc-audio-processing, позже speexdsp, rnnoise)
config/default.toml           конфигурация конвейера по умолчанию
src/core/                     Frame, RingBuffer, Timeline QPC<->sample, IStage, Chain, StageRegistry, config
src/stages/                   hpf, webrtc (AEC3 + hpf/ns/agc из APM), limiter; позже speex_aec, rnnoise, ...
src/wasapi/                   устройства, capture, loopback, render, уведомления (этап 2)
src/tools/                    apm_smoke; далее bomboec-rec (рекордер), bomboec-proc (офлайн-процессор)
src/app/                      tray-приложение (этап 4)
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
| 2. Рекордер | WASAPI mic в raw mode, loopback с keepalive, QPC-метки, multi-track WAV | синхронные записи с реального стола плюс metadata.json |
| 3. Офлайн-процессор | WAV в цепочку, WAV на выходе, ERLE и статистика APM | подобран конфиг AEC3 под конкретный setup |
| 4. Realtime | движок на mic-потоке, reference по таймлайну, вывод в VB-Cable, tray с диагностикой | Discord работает через виртуальный микрофон |
| 5. Устойчивость | уведомления устройств, перезапуск, контроллер заполнения выхода, измерение дрейфа, статический сдвиг reference | сутки работы без рассинхрона |
| 6. Второй бэкенд | рецепты speexdsp или rnnoise, стадии, опционально DLL-плагины | цепочка переключается через конфиг |
| 7. Драйвер | cable на SysVAD | только если VB-Cable перестанет устраивать |

## Бэкенды на будущее

AEC: WebRTC AEC3 (основной), SpeexDSP (дешёвый второй), LocalVQE (нейросетевой, 16 kHz, CPU).
NS: WebRTC NS, RNNoise, DeepFilterNet3, Maxine denoiser (GPU, через NGC).
