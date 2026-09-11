# Этап 0: smoke-тест линковки с WebRTC APM / AEC3.
add_executable(apm_smoke apm_smoke.cpp "${BOMBOEC_MANIFEST}")
target_link_libraries(apm_smoke PRIVATE bomboec::options webrtc-audio-processing::webrtc-audio-processing)

# Этап 2: синхронный рекордер mic + loopback.
add_executable(bomboec-rec rec.cpp console.h "${BOMBOEC_MANIFEST}")
target_link_libraries(bomboec-rec PRIVATE bomboec::options bomboec::core bomboec::wasapi cxxopts::cxxopts)

# Этап 3: офлайн-прогон записей через цепочку.
add_executable(bomboec-proc proc.cpp console.h "${BOMBOEC_MANIFEST}")
target_link_libraries(bomboec-proc PRIVATE bomboec::options bomboec::core bomboec::stages cxxopts::cxxopts)

# Этап 4: консольный запуск realtime-движка.
add_executable(bomboec-run run.cpp console.h "${BOMBOEC_MANIFEST}")
target_link_libraries(bomboec-run PRIVATE bomboec::options bomboec::engine bomboec::wasapi cxxopts::cxxopts)

# Тестовый сигнал в render endpoint (проверка кабеля, калибровка).
add_executable(bomboec-play play.cpp console.h "${BOMBOEC_MANIFEST}")
target_link_libraries(bomboec-play PRIVATE bomboec::options bomboec::core bomboec::wasapi cxxopts::cxxopts)
