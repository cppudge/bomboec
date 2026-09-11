add_executable(bomboec_tests
    test_frame.cpp
    test_ring_buffer.cpp
    test_timeline.cpp
    test_config.cpp
    test_chain.cpp
    test_stages.cpp
)
target_link_libraries(bomboec_tests PRIVATE bomboec_stages Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bomboec_tests)
