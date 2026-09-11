#pragma once

#include <cstdint>

namespace bomboec {

// Внутренний формат DSP-конвейера. Все стадии работают в нём; конвертация
// из/в форматы устройств выполняется на границах (WASAPI, WAV).
struct PipelineFormat {
    uint32_t sampleRate = 48000;
    uint32_t frameSamples = 480;   // 10 ms при 48 kHz
    uint32_t micChannels = 1;
    uint32_t referenceChannels = 2;

    double frameMs() const { return 1000.0 * frameSamples / sampleRate; }
};

}  // namespace bomboec
