# Ресурс версии (свойства файла в проводнике): версия из project() в корневом CMakeLists.txt.
configure_file(bomboec.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bomboec.rc" @ONLY)

add_executable(bomboec WIN32 main.cpp "${CMAKE_CURRENT_BINARY_DIR}/bomboec.rc" "${BOMBOEC_MANIFEST}")
target_link_libraries(bomboec PRIVATE bomboec::options bomboec::engine bomboec::wasapi shell32 user32 gdi32 ole32)

# version.h (версия и git describe для лога, окна статуса и ресурса) обновляется на каждой сборке.
set(BOMBOEC_VERSION_H "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")
add_custom_target(bomboec_version
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/version.h.in" "-DOUTPUT=${BOMBOEC_VERSION_H}"
            "-DVERSION=${PROJECT_VERSION}" -P "${PROJECT_SOURCE_DIR}/cmake/git_version.cmake"
    BYPRODUCTS "${BOMBOEC_VERSION_H}"
    VERBATIM)
add_dependencies(bomboec bomboec_version)
target_include_directories(bomboec PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
