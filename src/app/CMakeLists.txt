add_executable(bomboec WIN32 main.cpp)
target_link_libraries(bomboec PRIVATE bomboec::options bomboec::engine bomboec::wasapi shell32 user32 gdi32 ole32)
