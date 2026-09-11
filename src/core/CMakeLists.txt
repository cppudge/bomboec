add_library(bomboec_core STATIC
    audio_format.h
    frame.h
    ring_buffer.h
    timeline.h
    packet_assembler.h
    fill_controller.h
    seqlock.h
    denormals.h
    stage.h
    stage.cpp
    registry.h
    config.h
    config.cpp
    chain.h
    chain.cpp
    wav.h
    wav.cpp
    utf8.h
)
add_library(bomboec::core ALIAS bomboec_core)
target_include_directories(bomboec_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bomboec_core
    PUBLIC tomlplusplus::tomlplusplus
    PRIVATE bomboec::options dr_libs::dr_libs
)

# Шаблон конфигурации встраивается в код, config/default.toml остаётся единственным источником
# (defaultConfigToml() в config.cpp). Правка файла перезапускает настройку.
set(BOMBOEC_DEFAULT_CONFIG "${PROJECT_SOURCE_DIR}/config/default.toml")
file(READ "${BOMBOEC_DEFAULT_CONFIG}" BOMBOEC_DEFAULT_CONFIG_TEXT)
string(LENGTH "${BOMBOEC_DEFAULT_CONFIG_TEXT}" BOMBOEC_DEFAULT_CONFIG_LENGTH)
if(BOMBOEC_DEFAULT_CONFIG_LENGTH GREATER 15000)
    # Один строковый литерал MSVC вмещает около 16 KB (C2026).
    message(FATAL_ERROR "config/default.toml is ${BOMBOEC_DEFAULT_CONFIG_LENGTH} bytes, more than one MSVC "
                        "string literal holds: split the raw literal in src/core/default_config.inc.in")
endif()
configure_file(default_config.inc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/default_config.inc" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOMBOEC_DEFAULT_CONFIG}")
target_include_directories(bomboec_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
