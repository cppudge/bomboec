add_executable(bomboec_tests
    test_frame.cpp
    test_ring_buffer.cpp
    test_timeline.cpp
    test_packet_assembler.cpp
    test_wav.cpp
    test_config.cpp
    test_chain.cpp
    test_stages.cpp
    test_fill_controller.cpp
)
target_link_libraries(bomboec_tests PRIVATE bomboec::options bomboec::core bomboec::stages Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bomboec_tests)
