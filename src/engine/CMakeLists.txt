add_library(bomboec_engine STATIC
    pipeline.h
    pipeline.cpp
    engine.h
    engine.cpp
    watchdog.h
)
add_library(bomboec::engine ALIAS bomboec_engine)
target_include_directories(bomboec_engine PUBLIC ${PROJECT_SOURCE_DIR}/src)
# engine.h использует типы core и wasapi; стадии нужны только engine.cpp (реестр).
target_link_libraries(bomboec_engine
    PUBLIC bomboec::core bomboec::wasapi
    PRIVATE bomboec::options bomboec::stages
)
