add_executable(bomboec_tests
    fakes/fake_wasapi.h
    sim/pipeline_sim.h
    test_frame.cpp
    test_ring_buffer.cpp
    test_timeline.cpp
    test_packet_assembler.cpp
    test_wav.cpp
    test_config.cpp
    test_chain.cpp
    test_stages.cpp
    test_stage_params.cpp
    test_fill_controller.cpp
    test_wasapi_streams.cpp
    test_utf8.cpp
    test_devices.cpp
    test_watchdog.cpp
    test_pipeline.cpp
    test_pipeline_alignment.cpp
    test_realtime.cpp
    test_recorder.cpp
    test_crash_dump.cpp
    test_corpus.cpp
    "${BOMBOEC_MANIFEST}"
)
target_link_libraries(bomboec_tests PRIVATE
    bomboec::options bomboec::core bomboec::stages bomboec::wasapi bomboec::engine bomboec::crash
    Catch2::Catch2WithMain
)

# Корпус эталонных записей: манифест tests/corpus/corpus.toml, записи в recordings/ (вне git).
target_compile_definitions(bomboec_tests PRIVATE "BOMBOEC_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")

include(Catch)
catch_discover_tests(bomboec_tests)
