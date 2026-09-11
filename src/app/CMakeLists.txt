add_executable(bomboec WIN32 main.cpp)
target_link_libraries(bomboec PRIVATE bomboec_engine shell32 user32 gdi32 ole32)
