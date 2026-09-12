#include "stages/builtin_stages.h"

namespace bomboec {

void registerBuiltinStages(StageRegistry& registry) {
    registry.add("hpf", &makeHpfStage);
    registry.add("webrtc", &makeWebrtcStage);
    registry.add("rnnoise", &makeRnnoiseStage);
    registry.add("limiter", &makeLimiterStage);
    registry.add("transient", &makeTransientStage);
}

}  // namespace bomboec
