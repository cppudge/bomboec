# Этап 0: smoke-тест линковки с WebRTC APM / AEC3.
add_executable(apm_smoke apm_smoke.cpp)
target_link_libraries(apm_smoke PRIVATE webrtc-audio-processing::webrtc-audio-processing)
