add_library(bomboec_engine STATIC
    engine.h
    engine.cpp
)
target_link_libraries(bomboec_engine PUBLIC bomboec_core bomboec_stages bomboec_wasapi)
