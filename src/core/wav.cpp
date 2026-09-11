#include "core/wav.h"

#include "core/utf8.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include <dr_wav.h>

#include <cstdio>

namespace bomboec {

namespace {

// dr_wav без stdio: даём свои колбэки поверх FILE*, чтобы пути с юникодом
// открывались через _wfopen.
std::FILE* openFile(const std::filesystem::path& path, const wchar_t* mode) {
    std::FILE* f = nullptr;
    _wfopen_s(&f, path.wstring().c_str(), mode);
    return f;
}

size_t writeCb(void* user, const void* data, size_t bytes) {
    return std::fwrite(data, 1, bytes, static_cast<std::FILE*>(user));
}

size_t readCb(void* user, void* data, size_t bytes) {
    return std::fread(data, 1, bytes, static_cast<std::FILE*>(user));
}

drwav_bool32 seekCb(void* user, int offset, drwav_seek_origin origin) {
    return _fseeki64(static_cast<std::FILE*>(user), offset, origin == drwav_seek_origin_start ? SEEK_SET : SEEK_CUR) ==
           0;
}

}  // namespace

struct WavWriter::Impl {
    drwav wav{};
    std::FILE* file = nullptr;
    bool open = false;
};

WavWriter::WavWriter() : impl_(std::make_unique<Impl>()) {}
WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate, std::string& error) {
    close();
    impl_->file = openFile(path, L"wb");
    if (!impl_->file) {
        error = "cannot create " + pathToUtf8(path);
        return false;
    }
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = channels;
    fmt.sampleRate = sampleRate;
    fmt.bitsPerSample = 32;
    if (!drwav_init_write(&impl_->wav, &fmt, writeCb, seekCb, impl_->file, nullptr)) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
        error = "drwav_init_write failed for " + pathToUtf8(path);
        return false;
    }
    impl_->open = true;
    frames_ = 0;
    return true;
}

uint64_t WavWriter::write(const float* interleaved, uint64_t frames) {
    if (!impl_->open) return 0;
    const drwav_uint64 n = drwav_write_pcm_frames(&impl_->wav, frames, interleaved);
    frames_ += n;
    return n;
}

void WavWriter::close() {
    if (impl_->open) {
        drwav_uninit(&impl_->wav);
        impl_->open = false;
    }
    if (impl_->file) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

bool WavWriter::isOpen() const { return impl_->open; }

struct WavReader::Impl {
    drwav wav{};
    std::FILE* file = nullptr;
    bool open = false;
};

WavReader::WavReader() : impl_(std::make_unique<Impl>()) {}
WavReader::~WavReader() { close(); }

bool WavReader::open(const std::filesystem::path& path, std::string& error) {
    close();
    impl_->file = openFile(path, L"rb");
    if (!impl_->file) {
        error = "cannot open " + pathToUtf8(path);
        return false;
    }
    if (!drwav_init(&impl_->wav, readCb, seekCb, impl_->file, nullptr)) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
        error = "not a WAV file: " + pathToUtf8(path);
        return false;
    }
    impl_->open = true;
    channels_ = impl_->wav.channels;
    sampleRate_ = impl_->wav.sampleRate;
    totalFrames_ = impl_->wav.totalPCMFrameCount;
    return true;
}

uint64_t WavReader::read(float* interleaved, uint64_t frames) {
    if (!impl_->open) return 0;
    return drwav_read_pcm_frames_f32(&impl_->wav, frames, interleaved);
}

void WavReader::close() {
    if (impl_->open) {
        drwav_uninit(&impl_->wav);
        impl_->open = false;
    }
    if (impl_->file) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

bool readWavFile(const std::filesystem::path& path, std::vector<float>& interleaved, uint32_t& channels,
                 uint32_t& sampleRate, std::string& error) {
    WavReader r;
    if (!r.open(path, error)) return false;
    channels = r.channels();
    sampleRate = r.sampleRate();
    interleaved.resize(size_t(r.totalFrames() * channels));
    const uint64_t got = r.read(interleaved.data(), r.totalFrames());
    interleaved.resize(size_t(got * channels));
    return true;
}

}  // namespace bomboec
