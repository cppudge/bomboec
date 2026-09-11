#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bomboec {

// Запись WAV float32 (interleaved). Обёртка над dr_wav.
class WavWriter {
public:
    WavWriter();
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate, std::string& error);
    // Возвращает число записанных фреймов.
    uint64_t write(const float* interleaved, uint64_t frames);
    void close();
    bool isOpen() const;
    uint64_t framesWritten() const { return frames_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint64_t frames_ = 0;
};

// Чтение WAV любого поддерживаемого dr_wav формата с конвертацией в float32.
class WavReader {
public:
    WavReader();
    ~WavReader();
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);
    uint32_t channels() const { return channels_; }
    uint32_t sampleRate() const { return sampleRate_; }
    uint64_t totalFrames() const { return totalFrames_; }
    // Возвращает число прочитанных фреймов (меньше frames в конце файла).
    uint64_t read(float* interleaved, uint64_t frames);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    uint64_t totalFrames_ = 0;
};

// Читает файл целиком.
bool readWavFile(const std::filesystem::path& path, std::vector<float>& interleaved,
                 uint32_t& channels, uint32_t& sampleRate, std::string& error);

}  // namespace bomboec
