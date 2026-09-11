# Ресурс версии (свойства файла в проводнике): версия из project() в корневом CMakeLists.txt.
configure_file(bomboec.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bomboec.rc" @ONLY)

add_executable(bomboec WIN32 main.cpp "${CMAKE_CURRENT_BINARY_DIR}/bomboec.rc")
target_link_libraries(bomboec PRIVATE bomboec::options bomboec::engine bomboec::wasapi shell32 user32 gdi32 ole32)
