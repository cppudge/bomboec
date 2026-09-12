add_library(bomboec_wasapi STATIC
    com_util.h
    com_util.cpp
    devices.h
    devices.cpp
    capture_stream.h
    capture_stream.cpp
    render_stream.h
    render_stream.cpp
    device_notifier.h
    device_notifier.cpp
    sessions.h
    sessions.cpp
)
add_library(bomboec::wasapi ALIAS bomboec_wasapi)
target_include_directories(bomboec_wasapi PUBLIC ${PROJECT_SOURCE_DIR}/src)
# Обёртки WASAPI не зависят от ядра: только COM и Windows SDK.
target_link_libraries(bomboec_wasapi PRIVATE bomboec::options ole32 avrt propsys)
