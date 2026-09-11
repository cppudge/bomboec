add_library(bomboec_core STATIC
    audio_format.h
    frame.h
    ring_buffer.h
    timeline.h
    stage.h
    stage.cpp
    registry.h
    config.h
    config.cpp
    chain.h
    chain.cpp
)
target_include_directories(bomboec_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bomboec_core PUBLIC tomlplusplus::tomlplusplus)
