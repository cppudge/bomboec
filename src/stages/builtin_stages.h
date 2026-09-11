#pragma once

#include "core/registry.h"
#include "core/stage.h"

#include <memory>

namespace bomboec {

// Стадии, линкуемые статически. Каждая живёт в своём .cpp и не тянет
// сторонние заголовки в этот файл.
std::unique_ptr<IStage> makeHpfStage();      // id "hpf"
std::unique_ptr<IStage> makeWebrtcStage();   // id "webrtc": AEC3 (+ hpf/ns/agc из APM)
std::unique_ptr<IStage> makeLimiterStage();  // id "limiter"

void registerBuiltinStages(StageRegistry& registry);

}  // namespace bomboec
