#pragma once

#include <xmmintrin.h>

namespace bomboec {

// Denormal-числа в float DSP (хвосты фильтров после точной тишины, аппаратный
// mute) обрабатываются в десятки раз медленнее обычных. На время обработки
// включаются FTZ и DAZ (SSE, x64), прежний режим возвращается: DSP работает в
// колбэке потока WASAPI.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | kFtzDaz); }
    ~ScopedFlushDenormals() { _mm_setcsr(saved_); }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    static constexpr unsigned kFtzDaz = 0x8040;  // MXCSR: FTZ (бит 15) и DAZ (бит 6)
    unsigned saved_;
};

}  // namespace bomboec
