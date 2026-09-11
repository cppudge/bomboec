add_library(bomboec_stages STATIC
    builtin_stages.h
    builtin_stages.cpp
    hpf_stage.cpp
    limiter_stage.cpp
    webrtc_stage.cpp
)
target_link_libraries(bomboec_stages
    PUBLIC bomboec_core
    PRIVATE webrtc-audio-processing::webrtc-audio-processing
)
