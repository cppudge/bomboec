# Этап 0: smoke-тест линковки с WebRTC APM / AEC3.
add_executable(apm_smoke apm_smoke.cpp)
target_link_libraries(apm_smoke PRIVATE webrtc-audio-processing::webrtc-audio-processing)

# Этап 2: синхронный рекордер mic + loopback.
add_executable(bomboec-rec rec.cpp)
target_link_libraries(bomboec-rec PRIVATE bomboec_wasapi cxxopts::cxxopts)

# Этап 3: офлайн-прогон записей через цепочку.
add_executable(bomboec-proc proc.cpp)
target_link_libraries(bomboec-proc PRIVATE bomboec_stages cxxopts::cxxopts)

# Этап 4: консольный запуск realtime-движка.
add_executable(bomboec-run run.cpp)
target_link_libraries(bomboec-run PRIVATE bomboec_engine cxxopts::cxxopts)

# Тестовый сигнал в render endpoint (проверка кабеля, калибровка).
add_executable(bomboec-play play.cpp)
target_link_libraries(bomboec-play PRIVATE bomboec_wasapi cxxopts::cxxopts)
