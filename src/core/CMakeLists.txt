add_library(bomboec_core STATIC
    audio_format.h
    frame.h
    ring_buffer.h
    timeline.h
    packet_assembler.h
    stage.h
    stage.cpp
    registry.h
    config.h
    config.cpp
    chain.h
    chain.cpp
    wav.h
    wav.cpp
)
target_include_directories(bomboec_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bomboec_core
    PUBLIC tomlplusplus::tomlplusplus
    PRIVATE dr_libs::dr_libs
)
