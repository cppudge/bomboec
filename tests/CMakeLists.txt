add_executable(bomboec_tests
    fakes/fake_wasapi.h
    test_frame.cpp
    test_ring_buffer.cpp
    test_timeline.cpp
    test_packet_assembler.cpp
    test_wav.cpp
    test_config.cpp
    test_chain.cpp
    test_stages.cpp
    test_fill_controller.cpp
    test_wasapi_streams.cpp
    test_utf8.cpp
    test_devices.cpp
    test_watchdog.cpp
    "${BOMBOEC_MANIFEST}"
)
target_link_libraries(bomboec_tests PRIVATE
    bomboec::options bomboec::core bomboec::stages bomboec::wasapi bomboec::engine Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(bomboec_tests)
