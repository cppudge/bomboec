# Этап 0: smoke-тест линковки с WebRTC APM / AEC3.
add_executable(apm_smoke apm_smoke.cpp)
target_link_libraries(apm_smoke PRIVATE webrtc-audio-processing::webrtc-audio-processing)

# Этап 2: синхронный рекордер mic + loopback.
add_executable(bomboec-rec rec.cpp)
target_link_libraries(bomboec-rec PRIVATE bomboec_wasapi cxxopts::cxxopts)
