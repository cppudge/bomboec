#include "core/stage.h"

namespace bomboec {

const char* capName(Cap c) {
    switch (c) {
        case Cap::Hpf: return "hpf";
        case Cap::Aec: return "aec";
        case Cap::Ns: return "ns";
        case Cap::Agc: return "agc";
        case Cap::Limiter: return "limiter";
    }
    return "?";
}

}  // namespace bomboec
