add_library(bomboec_wasapi STATIC
    com_util.h
    com_util.cpp
    devices.h
    devices.cpp
    capture_stream.h
    capture_stream.cpp
    render_stream.h
    render_stream.cpp
)
target_link_libraries(bomboec_wasapi
    PUBLIC bomboec_core
    PRIVATE ole32 avrt propsys
)
