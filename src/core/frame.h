#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace bomboec {

// Planar float32 буфер фиксированного размера. Память выделяется в resize(),
// в hot path (process) аллокаций нет.
class Frame {
public:
    Frame() = default;
    Frame(uint32_t channels, uint32_t samples) { resize(channels, samples); }

    void resize(uint32_t channels, uint32_t samples) {
        channels_ = channels;
        samples_ = samples;
        data_.assign(size_t(channels) * samples, 0.0f);
        planes_.resize(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            planes_[c] = data_.data() + size_t(c) * samples;
        }
    }

    uint32_t channels() const { return channels_; }
    uint32_t samples() const { return samples_; }
    bool empty() const { return data_.empty(); }

    std::span<float> channel(uint32_t c) { return {planes_[c], samples_}; }
    std::span<const float> channel(uint32_t c) const { return {planes_[c], samples_}; }

    float* const* planes() { return planes_.data(); }
    const float* const* planes() const { return planes_.data(); }

    void clear() { std::fill(data_.begin(), data_.end(), 0.0f); }

    // Копия содержимого; размеры должны совпадать.
    void copyFrom(const Frame& other) { std::copy(other.data_.begin(), other.data_.end(), data_.begin()); }

    // Interleaved <-> planar. src/dst содержат channels()*samples() значений.
    void fromInterleaved(const float* src) {
        for (uint32_t c = 0; c < channels_; ++c) {
            float* dst = planes_[c];
            for (uint32_t i = 0; i < samples_; ++i) {
                dst[i] = src[size_t(i) * channels_ + c];
            }
        }
    }

    void toInterleaved(float* dst) const {
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* src = planes_[c];
            for (uint32_t i = 0; i < samples_; ++i) {
                dst[size_t(i) * channels_ + c] = src[i];
            }
        }
    }

private:
    uint32_t channels_ = 0;
    uint32_t samples_ = 0;
    std::vector<float> data_;
    std::vector<float*> planes_;
};

}  // namespace bomboec
