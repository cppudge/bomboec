# Windows AEC Application — Technical Design

## 1. Цель

Разработать приложение для Windows, которое удаляет из сигнала физического микрофона звук, воспроизводимый через системные колонки/наушники, и публикует очищенный сигнал как отдельный **Virtual Microphone**, доступный Discord, Zoom, Teams, браузеру, OBS и другим приложениям.

Главное требование: **не ухудшать качество воспроизводимого системного звука**. Приложение не должно заставлять Windows выводить звук через mono/16 kHz virtual speaker. Оригинальный playback идёт напрямую на выбранное физическое render-устройство в его штатном формате, а отдельная копия playback-потока используется как reference для AEC.

Поддерживаются два взаимозаменяемых AEC backend'а:

1. **WebRTC AEC3** — рекомендуемый backend по умолчанию.
2. **NVIDIA Maxine Audio Effects SDK AEC** — опциональный GPU backend.

Документ описывает общую архитектуру, Windows audio pipeline, синхронизацию потоков, калибровку задержки, virtual microphone, threading, buffering и особенности обоих AEC backend'ов.

---

## 2. Основной пользовательский сценарий

Пользователь выбирает:

- физический микрофон;
- физическое устройство воспроизведения;
- AEC backend: `WebRTC AEC3` или `NVIDIA Maxine AEC`;
- при необходимости Noise Suppression / AGC / дополнительные эффекты.

После запуска обработки приложение создаёт поток:

```text
Physical Microphone
        │
        ▼
     Capture
        │
        ├───────────────┐
        │               │
        ▼               ▼
       AEC          diagnostics
        │
        ▼
 optional NS/AGC
        │
        ▼
 Virtual Microphone
        │
        ├── Discord
        ├── Zoom
        ├── Teams
        ├── Browser
        └── OBS
```

Reference для AEC берётся независимо:

```text
Applications
    │
    ▼
Windows Audio Engine
    │
    ├──────────────────────────► Physical Speakers / DAC
    │                           original stereo quality
    │
    └── WASAPI Loopback ──────► AEC reference pipeline
```

Таким образом, AEC никогда не находится в основном render path.

---

## 3. Ключевой принцип: playback не проходит через virtual speaker

Не рекомендуется архитектура типа:

```text
Apps
 │
 ▼
Virtual Speaker 16 kHz mono
 │
 ▼
AEC application
 │
 ▼
Physical Speakers
```

Она приводит к тому, что Windows Audio Engine заранее преобразует весь системный звук в формат virtual endpoint'а. Если endpoint объявлен как mono 16 kHz, теряются stereo panorama и весь спектр выше примерно 8 kHz.

Вместо этого используется:

```text
                       ┌─────────────────────────────┐
                       │        Windows Audio        │
                       │           Engine            │
                       └──────────────┬──────────────┘
                                      │
                              native mix format
                             e.g. 48 kHz stereo
                                      │
                     ┌────────────────┴────────────────┐
                     │                                 │
                     ▼                                 ▼
             Physical Speakers                  WASAPI Loopback
             48/96 kHz stereo                  reference capture
             without modification                     │
                                                       ▼
                                                  AEC pipeline
```

WASAPI loopback специально позволяет capture-клиенту получать поток, воспроизводимый render endpoint'ом. Microsoft отдельно указывает AEC как один из основных сценариев loopback capture.

---

## 4. Рекомендуемый рабочий формат

Для внутреннего DSP pipeline рекомендуется:

```text
sample format: float32 planar/interleaved as required
sample rate:   48,000 Hz
mic channels:  1
render:        2 channels for AEC3
               1 mono reference for Maxine AEC
frame:         10 ms logical processing frame
```

При 48 kHz:

```text
10 ms = 480 samples/channel
```

48 kHz предпочтительнее 44.1 kHz для realtime voice DSP. WebRTC Audio Processing работает с логическими кадрами примерно 10 ms и для 48 kHz использует 480 samples/channel.

Физический output может при этом оставаться, например:

```text
96 kHz / 24-bit / stereo
```

WASAPI loopback получает формат Windows render engine, а reference pipeline при необходимости выполняет отдельный resampling в 48 kHz. Это **не влияет на звук, реально выводимый на устройство**.

---

## 5. High-level architecture

```text
┌─────────────────────────────────────────────────────────────────────┐
│                         Windows AEC App                             │
│                                                                     │
│  ┌────────────────┐       ┌──────────────────┐                      │
│  │ WASAPI Render  │       │ WASAPI Physical │                      │
│  │ Loopback       │       │ Mic Capture      │                      │
│  └───────┬────────┘       └────────┬─────────┘                      │
│          │                         │                                │
│          ▼                         ▼                                │
│  ┌────────────────┐       ┌──────────────────┐                      │
│  │ Format Convert │       │ Format Convert   │                      │
│  │ / Resampler    │       │ / Resampler      │                      │
│  └───────┬────────┘       └────────┬─────────┘                      │
│          │                         │                                │
│          └───────────┬─────────────┘                                │
│                      ▼                                              │
│             ┌──────────────────┐                                    │
│             │ Sync / Delay     │                                    │
│             │ Alignment Layer  │                                    │
│             └────────┬─────────┘                                    │
│                      ▼                                              │
│             ┌──────────────────┐                                    │
│             │ IAecBackend      │                                    │
│             └───────┬──────────┘                                    │
│                     │                                               │
│          ┌──────────┴──────────┐                                    │
│          ▼                     ▼                                    │
│  ┌──────────────┐      ┌─────────────────┐                          │
│  │ WebRTC AEC3  │      │ NVIDIA Maxine  │                          │
│  │ backend      │      │ AEC backend     │                          │
│  └──────────────┘      └─────────────────┘                          │
│                     │                                               │
│                     ▼                                               │
│             ┌──────────────────┐                                    │
│             │ Optional DSP     │                                    │
│             │ NS / HPF / AGC   │                                    │
│             └────────┬─────────┘                                    │
│                      ▼                                              │
│             ┌──────────────────┐                                    │
│             │ Virtual Mic      │                                    │
│             │ Output Bridge    │                                    │
│             └──────────────────┘                                    │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Компоненты приложения

Предлагаемая структура C++ проекта:

```text
src/
├── audio/
│   ├── wasapi_capture.cpp
│   ├── wasapi_loopback.cpp
│   ├── device_manager.cpp
│   ├── audio_format.cpp
│   ├── resampler.cpp
│   ├── channel_converter.cpp
│   ├── ring_buffer.cpp
│   └── audio_clock.cpp
│
├── sync/
│   ├── stream_aligner.cpp
│   ├── delay_estimator.cpp
│   ├── drift_estimator.cpp
│   └── calibration.cpp
│
├── aec/
│   ├── iaec_backend.h
│   ├── webrtc_aec3_backend.cpp
│   └── maxine_aec_backend.cpp
│
├── output/
│   ├── virtual_mic_client.cpp
│   └── virtual_mic_protocol.h
│
├── service/
│   ├── audio_engine.cpp
│   ├── settings.cpp
│   └── telemetry.cpp
│
└── ui/
    └── ...

driver/
└── virtual_microphone/
    └── ...
```

Основной engine не должен зависеть от конкретного AEC backend'а.

---

## 7. AEC backend abstraction

Минимальный interface:

```cpp
struct AecFormat {
    uint32_t sampleRate;
    uint32_t renderChannels;
    uint32_t captureChannels;
    uint32_t framesPerBlock;
};

class IAecBackend {
public:
    virtual ~IAecBackend() = default;

    virtual bool initialize(const AecFormat& format) = 0;
    virtual void reset() = 0;

    // Reference: what is being sent to speakers.
    virtual void pushRenderFrame(const float* const* render) = 0;

    // Mic in -> cleaned mic out.
    virtual void processCaptureFrame(
        const float* const* input,
        float* const* output) = 0;

    virtual void setEstimatedDelayMs(float delayMs) = 0;
};
```

Backend-specific details должны быть скрыты внутри реализации.

---

# Part I — Windows Audio Input

## 8. Physical microphone capture

Использовать Windows Core Audio / WASAPI.

Основные интерфейсы:

```text
IMMDeviceEnumerator
IMMDevice
IAudioClient / IAudioClient3
IAudioCaptureClient
```

Для микрофона:

```text
IMMDeviceEnumerator
       │
       ▼
GetDefaultAudioEndpoint(eCapture, ...)
       │
       ▼
IAudioClient
       │
       ▼
shared mode / event driven
       │
       ▼
IAudioCaptureClient
```

Рекомендуется event-driven capture вместо busy polling.

Приложение должно уметь работать с любым типичным endpoint format:

```text
44.1 kHz stereo
48 kHz mono
48 kHz stereo
96 kHz mono
...
```

После capture формат нормализуется во внутренний DSP format.

Для обычного desktop microphone можно использовать:

```text
48 kHz float32 mono
```

Если физический microphone stereo/array — в первой версии допустимо downmix/выбор одного канала. Позже можно добавить multi-capture AEC3.

---

## 9. Render reference через WASAPI Loopback

Получаем выбранный physical render endpoint:

```cpp
GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
```

Затем инициализируем capture stream на render endpoint с:

```text
AUDCLNT_STREAMFLAGS_LOOPBACK
```

Схема:

```text
Chrome / Game / Spotify / Discord
              │
              ▼
       Windows Audio Engine
              │
       ┌──────┴──────┐
       │             │
       ▼             ▼
 Physical output   Loopback capture
```

Loopback работает в WASAPI shared mode. Exclusive render streams требуют отдельного рассмотрения и могут не попадать в обычный system mix.

### Почему это лучше virtual speaker

Пользователь продолжает выбирать свои реальные колонки как normal Windows output device.

Приложение лишь подписывается на копию render mix.

Следовательно:

```text
Playback fidelity = fidelity без установленного AEC приложения
```

за исключением стандартного поведения Windows shared audio engine.

---

## 10. Process-specific loopback — опционально

Windows также позволяет захватывать render audio конкретного процесса и его descendants через application loopback capture.

Это может быть полезно для режима:

```text
Cancel only Discord audio
```

или:

```text
Cancel everything except music player
```

Однако для обычного desktop AEC рекомендуется system endpoint loopback, потому что физический микрофон слышит сумму всего, что реально играет через колонки.

---

# Part II — Stream Synchronization

## 11. Почему синхронизация критична

AEC решает задачу приблизительно вида:

```text
mic(t) = voice(t) + acoustic_path(render(t - D)) + noise(t)
```

где `D` — effective render-to-capture delay.

В него входят:

```text
render buffering
+ Windows Audio Engine latency
+ device/driver latency
+ DAC latency
+ acoustic propagation
+ microphone ADC latency
+ capture buffering
```

Если reference и microphone сильно смещены относительно друг друга, adaptive filter не сможет корректно моделировать echo path.

---

## 12. Не полагаться только на arrival time потоков

Нельзя делать:

```text
очередной loopback buffer + очередной mic buffer -> AEC
```

без временной модели.

Два WASAPI endpoint'а могут иметь:

- разные buffer sizes;
- разные hardware clocks;
- variable scheduling latency;
- clock drift;
- device changes;
- временные underrun/overrun.

Нужен StreamAligner.

---

## 13. Timestamp model

Для каждого блока желательно хранить:

```cpp
struct TimedAudioBlock {
    uint64_t qpcTimestamp;
    uint64_t devicePosition;
    uint32_t frames;
    AudioBuffer audio;
};
```

Использовать:

- WASAPI device position/timestamps, где доступны;
- `QueryPerformanceCounter` как общий high-resolution host clock;
- собственные cumulative sample counters.

Цель — сопоставлять положение render/reference и mic capture во времени, а не только порядок callback'ов.

---

## 14. Initial calibration mode

Рекомендуется отдельный режим калибровки.

Алгоритм:

1. Приложение воспроизводит известный calibration signal через выбранный physical output.
2. Loopback получает точную digital reference копию.
3. Microphone записывает сигнал после прохождения DAC → speakers → room → mic.
4. Программа ищет reference signal в mic capture.
5. Рассчитывается initial delay.

Схема:

```text
reference
───────██████────────────────────────────

microphone
────────────────██████───────────────────
                <---->
                  D
```

Подходящие calibration signals:

- logarithmic chirp;
- MLS sequence;
- pseudo-random noise burst;
- короткий broadband pulse при безопасной громкости.

Простейший estimator:

```text
D = argmax cross_correlation(reference, microphone)
```

Для realtime реализации эффективнее FFT-based cross correlation / GCC-PHAT.

### Важно

Calibration result — это **initial estimate**, а не вечная константа.

Драйверы, buffer periods и clocks могут немного изменяться. Backend AEC и StreamAligner должны продолжать адаптацию во время работы.

---

## 15. Clock drift

Даже если оба устройства nominally 48 kHz:

```text
render device actual clock  ≈ 48000.8 Hz
capture device actual clock ≈ 47999.4 Hz
```

Через длительное время возникнет sample drift.

Необходимо мониторить разницу накопленных timestamps/sample positions.

При превышении threshold выполнять очень небольшую динамическую коррекцию reference stream:

```text
adaptive resampling ratio
≈ 0.9999 ... 1.0001
```

Нельзя периодически резко удалять/дублировать большие куски: это создаёт discontinuity и ухудшает convergence AEC.

---

# Part III — WebRTC AEC3 Backend

## 16. Когда выбирать AEC3

AEC3 рекомендуется как default backend, потому что:

- не требует NVIDIA GPU;
- CPU-only;
- подходит для realtime communications;
- поддерживает 48 kHz;
- имеет delay estimation/adaptation;
- поддерживает multi-channel render processing;
- имеет residual echo suppression;
- широко используется в WebRTC stack.

---

## 17. AEC3 audio model

Для этого приложения предпочтительная конфигурация:

```text
render input:  48 kHz, stereo
capture input: 48 kHz, mono
output:        48 kHz, mono
frame:         10 ms / 480 samples
```

Pipeline:

```text
WASAPI Loopback L ─────────────┐
                               │
WASAPI Loopback R ─────────────┼──► AEC3 render path
                               │
Physical Mic ──────────────────┴──► AEC3 capture path
                                         │
                                         ▼
                                  cleaned mono mic
```

Current AEC3 source содержит multi-channel content detection: backend может использовать несколько render input channels при наличии настоящего multichannel content и сводить effective-mono material к одной reference ветке.

Это полезнее безусловного `L + R` downmix, потому что левый и правый loudspeaker имеют разные acoustic transfer functions до микрофона.

---

## 18. Использовать AudioProcessing API или EchoCanceller3 напрямую

Есть два подхода.

### Вариант A — WebRTC AudioProcessing API

Предпочтителен для production integration.

```text
AudioProcessing
 ├── Echo Canceller
 ├── High-pass filter
 ├── Noise suppression (optional)
 └── Gain controller (optional)
```

Типичный порядок:

```text
ProcessReverseStream(render)
ProcessStream(microphone)
```

`ProcessReverseStream` получает far-end/render reference.

WebRTC API ожидает примерно 10 ms frames. При 48 kHz это 480 samples/channel.

### Вариант B — EchoCanceller3 напрямую

Даёт больше контроля над AEC3-specific configuration и diagnostics, но сильнее связывает приложение с внутренними API WebRTC, которые менее стабильны, чем public AudioProcessing surface.

Рекомендация:

```text
MVP / production abstraction:
WebRTC AudioProcessing API

Research / tuning build:
optional direct EchoCanceller3 adapter
```

---

## 19. AEC3 frame processing

Application audio callbacks не обязаны приходить ровно по 480 samples.

Например:

```text
WASAPI callback:  192 samples
next callback:    288 samples
```

FrameAccumulator собирает их:

```text
192 + 288 = 480
              │
              ▼
        one 10 ms AEC frame
```

Для render и capture нужны отдельные accumulator/ring buffers.

AEC thread работает строго в своём fixed logical frame cadence.

---

## 20. AEC3 multichannel render

В отличие от Maxine варианта ниже, для AEC3 не рекомендуется заранее превращать stereo render в mono.

```text
Reference #0 = Left
Reference #1 = Right
```

Conceptually echo model:

```text
echo ~= H_L * Left + H_R * Right
```

При очень коррелированном stereo material задача идентификации отдельных acoustic paths становится сложнее — это фундаментальное ограничение stereophonic AEC. Поэтому dynamic multichannel detection/fallback полезен.

---

## 21. Delay hint

Если используется high-level WebRTC AudioProcessing interface, следует передавать доступную информацию о stream delay через соответствующий supported API конкретной pinned WebRTC revision.

Важно: WebRTC APIs меняются, поэтому проект должен фиксировать конкретный WebRTC commit/version и не строить ABI-зависимость на системной библиотеке.

Калибровочный `D` используется как initial hint/diagnostic, а не как единственный механизм alignment.

---

# Part IV — NVIDIA Maxine AEC Backend

## 22. Когда выбирать Maxine

Maxine AEC имеет смысл как optional backend если:

- на машине гарантирован NVIDIA GPU с Tensor Cores;
- допускается NVIDIA SDK/runtime dependency;
- нужны NVIDIA audio effects в том же pipeline;
- измерения на целевом hardware показывают лучший subjective/result quality.

Приложение всё равно должно иметь CPU fallback — AEC3.

---

## 23. Hardware/runtime requirements

Текущий NVIDIA Audio Effects SDK for Windows требует NVIDIA GPU с Tensor Cores. Актуальная документация указывает Windows x64 и достаточно новый NVIDIA driver; точную минимальную версию нужно проверять для выбранной версии SDK при release packaging.

Нельзя предполагать наличие Maxine backend на любой машине.

Startup logic:

```text
Is NVIDIA AFX runtime present?
          │
    ┌─────┴─────┐
    │           │
   yes          no
    │           │
GPU supported?  └──► AEC3
    │
 ┌──┴──┐
 │     │
yes    no
 │     │
 ▼     └────────► AEC3
Maxine
```

---

## 24. Критическое отличие Maxine AEC

У NVIDIA AEC API два input channels означают:

```text
channel 0 = near-end microphone
channel 1 = far-end/reference audio
```

Это **не stereo render reference**.

Следовательно, при stereo Windows playback Maxine path должен сделать внутренний downmix:

```text
               Left ──┐
                      ├──► internal stereo→mono ─► far-end input
               Right ─┘

Physical playback L/R remains untouched.
```

То есть:

```text
Physical speakers: stereo 48/96 kHz unchanged
AEC reference:     mono 48 kHz
```

---

## 25. Stereo-to-mono reference для Maxine

Начальный вариант:

```text
mono = 0.5 * L + 0.5 * R
```

Следует предусмотреть headroom, чтобы correlated L/R не создавали clipping.

Лучше работать в float32 и ограничивать/нормализовать только при необходимости.

В дальнейшем можно экспериментировать с weighted downmix, но для физически симметричного desktop setup обычный equal-weight downmix — хороший baseline.

---

## 26. Maxine sample rate

Current AFX documentation содержит AEC models/flows как минимум для 16 kHz и 48 kHz.

Для этого приложения использовать:

```text
AEC sample rate = 48 kHz
```

16 kHz не нужен, если нет отдельного требования low-bandwidth voice mode.

Снижение reference до 16 kHz не влияет на physical playback, но уменьшает спектральную информацию для AEC и здесь не даёт очевидного преимущества.

---

## 27. Maxine effect lifecycle

Conceptual initialization:

```cpp
NvAFX_Handle effect = nullptr;
NvAFX_CreateEffect(NVAFX_EFFECT_AEC, &effect);

// Configure/load feature/model according to the pinned SDK version.
// Query supported rates/channels/frame sizes.

NvAFX_Load(effect);
```

Перед началом realtime processing приложение обязано query'ить:

```text
NVAFX_PARAM_NUM_INPUT_CHANNELS
NVAFX_PARAM_NUM_OUTPUT_CHANNELS
NVAFX_PARAM_INPUT_SAMPLE_RATE
NVAFX_PARAM_OUTPUT_SAMPLE_RATE
NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME
NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME
```

Нельзя hardcode'ить frame size без проверки выбранной версии AFX/model.

Run:

```cpp
NvAFX_Run(effect, input, output, numSamples, numChannels);
```

AEC input layout формируется в соответствии с API выбранной версии SDK:

```text
near-end mic + far-end mono reference
```

---

## 28. Dynamic loading Maxine

Рекомендуется не делать NVIDIA runtime обязательной dependency всего приложения.

Использовать:

```text
LoadLibraryW("NVAudioEffects.dll")
GetProcAddress(...)
```

или отдельный backend DLL:

```text
aec_backend_maxine.dll
```

Тогда основной executable запускается и на системе без NVIDIA software.

Например:

```text
AecApp.exe
 ├── aec_backend_webrtc.dll
 └── aec_backend_maxine.dll   optional
```

---

# Part V — Common DSP Pipeline

## 29. Recommended processing order

Базовый вариант:

```text
microphone
    │
    ▼
DC / high-pass filter
    │
    ▼
AEC
    │
    ▼
Noise Suppression (optional)
    │
    ▼
AGC / limiter (optional)
    │
    ▼
Virtual Microphone
```

Не рекомендуется агрессивно denoise'ить microphone **до AEC**, если конкретный backend не рассчитан на такой preprocessing. Изменение near-end/echo signal перед adaptive cancellation может ухудшить echo model.

Render reference также желательно давать максимально близким к реально воспроизводимому digital signal.

---

## 30. Не применять volume control дважды

Reference должен соответствовать тому signal level, который реально был отправлен Windows Audio Engine на render endpoint.

Если loopback уже содержит effective system/device volume processing, не надо вручную второй раз умножать reference на Windows volume setting.

---

## 31. Device change handling

Следить за:

```text
default render changed
render endpoint unplugged
capture endpoint unplugged
sample format changed
exclusive-mode conflict
Bluetooth profile changed
USB device re-enumerated
```

При significant echo-path change:

```text
1. stop callbacks
2. recreate affected WASAPI client
3. clear ring buffers
4. reset/reinitialize AEC state
5. recalculate delay
6. resume output
```

Если пользователь переключил speakers, старый adaptive filter нельзя считать валидным.

---

# Part VI — Virtual Microphone

## 32. Что требуется от virtual microphone

В Windows приложения должны видеть результат как обычный capture endpoint:

```text
AEC Virtual Microphone
```

Он должен появляться в:

```text
Settings → System → Sound → Input
Discord → Input Device
Zoom → Microphone
Chrome getUserMedia()
OBS → Audio Input Capture
```

Один только user-mode WASAPI client не может магически создать универсальный microphone endpoint для других приложений. Для полноценного продукта обычно нужен virtual audio driver либо существующий third-party virtual audio transport.

---

## 33. Production вариант: собственный virtual audio driver

Отправная точка Microsoft — **SysVAD Virtual Audio Device Driver Sample**.

Архитектура:

```text
                 User mode
┌────────────────────────────────────┐
│ AEC Engine                         │
│                                    │
│ cleaned float32/PCM                │
│        │                           │
│        ▼                           │
│ VirtualMicWriter                   │
└────────┬───────────────────────────┘
         │ shared ring / IOCTL / protocol
═════════╪════════ kernel boundary ═════════
         │
┌────────▼───────────────────────────┐
│ Virtual Audio Driver              │
│                                   │
│ WaveRT capture endpoint           │
│ "AEC Virtual Microphone"         │
└────────┬───────────────────────────┘
         │
         ▼
Windows Audio Engine
         │
         ▼
Discord / Zoom / Browser / OBS
```

Driver component должен быть минимальным. DSP лучше оставить в user mode.

Причины:

- безопаснее;
- проще обновлять AEC backend;
- Maxine работает в user mode/GPU runtime;
- crash DSP процесса не должен crash'ить kernel.

---

## 34. MVP без собственного драйвера

Чтобы сначала проверить качество AEC, можно использовать существующий virtual audio cable и писать cleaned signal в его render side, который другой стороне экспонируется как capture endpoint.

MVP:

```text
AEC App output
      │
      ▼
existing virtual cable
      │
      ▼
Discord etc.
```

Это позволяет отделить две задачи:

1. добиться качественного AEC;
2. затем написать/sign/package собственный Windows audio driver.

Для research prototype это настоятельно рекомендуется.

---

# Part VII — Threading и realtime constraints

## 35. Threads

Минимально:

```text
Thread 1: WASAPI render loopback capture
Thread 2: WASAPI microphone capture
Thread 3: DSP/AEC processing
Thread 4: virtual microphone writer
Thread 5: UI/control/telemetry
```

WASAPI callback threads не должны выполнять тяжёлый DSP.

Capture callback:

```text
read packet
copy/move to preallocated ring buffer
signal consumer
return
```

AEC thread выполняет processing.

---

## 36. Не аллоцировать память в hot path

Realtime path должен избегать:

```text
new/delete
std::vector growth
filesystem access
logging with locks
network calls
GPU model loading
COM device enumeration
```

Использовать:

- заранее выделенные buffers;
- bounded lock-free/SPSC queues там, где они оправданы;
- object pools;
- background diagnostics writer.

---

## 37. Ring buffer sizing

Нужно выдерживать scheduling jitter, но нельзя добавлять сотни ms лишней задержки.

Пример starting point:

```text
render reference ring: 200–500 ms capacity
mic capture ring:      100–250 ms capacity
processed output ring: 50–100 ms capacity
```

Это capacity, а не target buffered latency.

Target processing queue depth должен быть минимальным — несколько frames.

---

## 38. Underrun / overrun policy

### Render reference underrun

Если для mic frame отсутствует matching reference:

- не использовать произвольный старый frame;
- отметить discontinuity;
- backend при необходимости reset/soft-reset;
- временно pass-through mic лучше, чем обрабатывать его неправильным reference.

### Ring overrun

Сбрасывать oldest data и инициировать resynchronization. AEC со reference, отстающим на секунду, бессмысленен.

---

# Part VIII — Latency

## 39. Что влияет на end-to-end mic latency

```text
mic capture period
+ frame accumulation
+ alignment buffer
+ AEC processing
+ optional NS/AGC
+ virtual microphone buffering
+ consumer app buffering
```

Ориентир для приложения: собственная добавленная latency должна оставаться достаточно низкой для voice communication.

AEC pipeline с 10 ms logical frames не означает автоматически ровно 10 ms total latency; задержка зависит от buffering strategy и backend internals.

---

## 40. Не задерживать physical playback ради AEC без необходимости

Архитектура должна стараться двигать/align'ить **reference относительно capture**, а не добавлять искусственную задержку всему системному playback.

Иначе приложение будет портить gaming/video experience.

---

# Part IX — Diagnostics

## 41. Что отображать пользователю

Полезный realtime diagnostics panel:

```text
Input mic:           Shure MV7
Render reference:    Speakers (Realtek USB DAC)
Backend:             WebRTC AEC3
DSP rate:            48 kHz
Render channels:     2
Capture channels:    1
Estimated delay:     42.6 ms
Clock drift:         +8 ppm
Reference level:     -18.3 dBFS
Mic level:           -24.1 dBFS
Output level:        -28.7 dBFS
Buffer health:       OK
AEC state:           converged
```

---

## 42. Debug recording mode

Очень желательно иметь opt-in developer mode, записывающий синхронизированные tracks:

```text
01_render_L.wav
02_render_R.wav
03_mic_raw.wav
04_mic_aec.wav
05_mic_final.wav
metadata.json
```

`metadata.json`:

```json
{
  "sample_rate": 48000,
  "backend": "webrtc_aec3",
  "estimated_delay_ms": 42.6,
  "render_device": "...",
  "capture_device": "...",
  "underruns": 0,
  "overruns": 0
}
```

Это критически полезно для offline comparison AEC3 vs Maxine и воспроизводимых bug reports.

Debug recording должен быть явно включаемым пользователем, так как содержит микрофонный и системный звук.

---

# Part X — Backend Comparison

## 43. AEC3 vs Maxine

| Характеристика | WebRTC AEC3 | NVIDIA Maxine AEC |
|---|---|---|
| GPU required | Нет | Да, NVIDIA Tensor Core GPU |
| CPU-only fallback | Да | Нет |
| 48 kHz | Да | Да |
| Stereo/multi-render reference | Да, AEC3 имеет multichannel render path | Нет в смысле L/R references; AEC input channels — near + far |
| Reference preparation | L/R можно сохранить отдельно | L/R нужно свести в mono reference |
| Runtime distribution | Нужно собирать/pin'ить WebRTC | NVIDIA AFX runtime/features |
| Vendor dependency | Нет | NVIDIA |
| Optional NVIDIA AI effects | Нет | Да |
| Recommended default | **Да** | Optional acceleration/quality backend |

Важно: качество нужно сравнивать на реальных сценариях, а не выбирать Maxine только потому, что он GPU/AI-based.

---

# Part XI — Recommended implementation strategy

## 44. Phase 1 — Offline AEC harness

До realtime Windows integration сделать utility:

```text
aec_test.exe
    --mic mic.wav
    --render render.wav
    --backend webrtc|maxine
    --output cleaned.wav
```

Для stereo AEC3:

```text
render_L.wav + render_R.wav
```

Для Maxine:

```text
render mono downmix
```

Метрики:

- ERLE;
- residual echo level;
- speech distortion;
- double-talk quality;
- convergence time;
- subjective listening.

---

## 45. Phase 2 — Realtime WASAPI capture

Добавить:

```text
WASAPI mic
WASAPI loopback
48 kHz normalization
10 ms framing
basic timestamp logging
```

Пока output можно писать в WAV.

---

## 46. Phase 3 — Realtime AEC output через existing virtual cable

```text
mic + loopback
      │
      ▼
AEC3 / Maxine
      │
      ▼
third-party virtual cable
      │
      ▼
Discord
```

На этом этапе можно полноценно измерять usability без разработки driver'а.

---

## 47. Phase 4 — Calibration and drift correction

Добавить:

- calibration chirp;
- GCC-PHAT/cross-correlation estimator;
- timestamp alignment;
- drift estimator;
- adaptive resampling;
- automatic AEC reset при device topology change.

---

## 48. Phase 5 — Own Virtual Microphone

На основе SysVAD/Windows audio driver architecture:

- expose capture endpoint;
- user-mode → driver ring protocol;
- driver signing;
- installer;
- clean uninstall/update.

---

## 49. Phase 6 — Product hardening

Добавить:

- startup with Windows;
- device hot-plug handling;
- Bluetooth transitions;
- suspend/resume;
- backend fallback;
- crash recovery;
- watchdog;
- diagnostics;
- automatic latency calibration;
- privacy-safe logs;
- regression audio test suite.

---

# Part XII — Suggested configuration

## 50. Default mode

```yaml
engine:
  sample_rate: 48000
  frame_ms: 10

capture:
  channels: 1
  mode: shared
  event_driven: true

render_reference:
  source: wasapi_loopback
  preserve_physical_playback: true

sync:
  calibration: automatic
  drift_compensation: true

backend:
  preferred: webrtc_aec3
  fallback: webrtc_aec3

webrtc_aec3:
  preserve_stereo_reference: true

maxine:
  reference_channels: 1
  stereo_downmix: equal_power_or_safe_average

output:
  device: "AEC Virtual Microphone"
  sample_rate: 48000
  channels: 1
```

---

# Part XIII — Failure modes to test

## 51. Required test matrix

### Acoustic cases

```text
speaker volume low / medium / maximum
mic 20 cm / 50 cm / 1 m from speakers
quiet room
reverberant room
near-end speech while far-end speech plays
game/music playback
highly stereo content
mostly mono content
```

### Windows cases

```text
44.1 kHz render endpoint
48 kHz endpoint
96 kHz endpoint
USB DAC reconnect
default device switch
headphones → speakers
Bluetooth reconnect
sleep → resume
consumer app opens/closes virtual mic
```

### DSP cases

```text
silence
render-only
mic-only
double-talk
impulsive sound
clipping
clock drift
reference dropout
mic dropout
100+ ms temporary scheduling stall
```

---

# Part XIV — Security and privacy

## 52. Audio privacy

Приложение имеет доступ одновременно к:

- microphone;
- system playback.

Поэтому production build должен:

- не записывать raw audio без явного opt-in;
- не отправлять audio в сеть без отдельной функции и consent;
- хранить diagnostics audio только локально;
- показывать состояние capture;
- очищать temporary recordings;
- иметь понятную privacy policy.

AEC3 может работать полностью локально CPU-side. Maxine AFX также рассчитан на локальную client-side обработку на поддерживаемой NVIDIA GPU.

---

# Part XV — Итоговая рекомендуемая архитектура

## 53. Production target

```text
                                  WINDOWS

        ┌────────────────────────────────────────────────────┐
        │                                                    │
Apps ───┼──► Windows Audio Engine ─────► Physical Speakers   │
        │             │                  native stereo       │
        │             │                                     │
        │             └──► WASAPI Loopback                   │
        │                        │                           │
        │                        ▼                           │
        │                 Reference FIFO                     │
        │                        │                           │
        │        ┌───────────────┴───────────────┐           │
        │        │                               │           │
        │        │ AEC3                          │ Maxine    │
        │        │ L + R refs                    │ L/R      │
        │        │                               │ downmix  │
        │        └───────────────┬───────────────┘           │
        │                        │                           │
Mic ────┼──► WASAPI Capture ────┼──► Sync/Align ─► AEC      │
        │                        │                    │       │
        │                        │                    ▼       │
        │                        │               Optional DSP│
        │                        │                    │       │
        │                        │                    ▼       │
        │                        │             Virtual Mic   │
        │                        │                    │       │
        └────────────────────────┼────────────────────┼───────┘
                                 │                    │
                                 │                    ▼
                                 │           Discord / Zoom /
                                 │           Teams / Browser
                                 ▼
                         Calibration / Metrics
```

### Рекомендуемый baseline

```text
Windows:        WASAPI shared/event-driven
DSP:            48 kHz float32
Frame cadence:  10 ms
Render:         original physical output unchanged
Reference:      WASAPI loopback
Mic:            mono
AEC default:    WebRTC AEC3
AEC3 reference: stereo where available
AEC optional:   NVIDIA Maxine AEC
Maxine ref:     internal mono downmix only
Output:         48 kHz mono Virtual Microphone
Calibration:    automatic cross-correlation + runtime drift tracking
```

Это позволяет получить Krisp-подобный пользовательский сценарий без главного недостатка virtual-speaker архитектуры: **качество системного playback не ограничивается форматом, необходимым AEC**.

---

# Sources / implementation references

1. Microsoft — WASAPI Loopback Recording:  
   https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording

2. Microsoft — WASAPI / Core Audio interfaces:  
   https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi

3. Microsoft — Application loopback audio capture sample:  
   https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/

4. Microsoft — SysVAD Virtual Audio Device Driver Sample:  
   https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/sysvad-virtual-audio-device-driver-sample/

5. WebRTC — current AudioProcessing API / frame sizing:  
   https://webrtc.googlesource.com/src/+/refs/heads/main/api/audio/audio_processing.h

6. WebRTC — AEC3 EchoCanceller3 implementation / multichannel render handling:  
   https://webrtc.googlesource.com/src/+/refs/heads/main/modules/audio_processing/aec3/echo_canceller3.cc

7. NVIDIA — Audio Effects (AFX) SDK User Guide:  
   https://docs.nvidia.com/maxine/afx/latest/

8. NVIDIA — Acoustic Echo Cancellation Effect:  
   https://docs.nvidia.com/maxine/afx/latest/AboutTheEffects/AboutAcousticEchoCancellation.html

9. NVIDIA — Load, Run and Destroy an Audio Effect / AEC channel semantics:  
   https://docs.nvidia.com/maxine/afx/latest/UseAFXInApps/LoadRunDestroyAnEffect.html

10. NVIDIA — Get Parameters of an Audio Effect:  
    https://docs.nvidia.com/maxine/afx/latest/UseAFXInApps/GetParametersOfAnEffect.html

11. NVIDIA — Windows AFX SDK requirements:  
    https://docs.nvidia.com/maxine/afx/latest/WindowsAFXSDK/GetStartedOnWindows.html

---

## Notes on version pinning

WebRTC internal APIs и NVIDIA AFX SDK могут изменяться. Перед началом production implementation следует зафиксировать:

```text
WebRTC commit SHA
NVIDIA AFX SDK version
Windows SDK / WDK version
minimum supported Windows build
MSVC toolset version
```

и запускать audio regression tests при каждом обновлении любой из этих зависимостей.

