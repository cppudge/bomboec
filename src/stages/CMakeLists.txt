add_library(bomboec_stages STATIC
    builtin_stages.h
    builtin_stages.cpp
    hpf_stage.cpp
    limiter_stage.cpp
    webrtc_stage.cpp
    rnnoise_stage.cpp
    transient_gate.h
    transient_gate.cpp
    transient_stage.cpp
)
add_library(bomboec::stages ALIAS bomboec_stages)
target_include_directories(bomboec_stages PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bomboec_stages
    PUBLIC bomboec::core
    PRIVATE bomboec::options webrtc-audio-processing::webrtc-audio-processing rnnoise::rnnoise
)
