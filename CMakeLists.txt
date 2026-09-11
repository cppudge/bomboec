cmake_minimum_required(VERSION 3.28)
project(bomboec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES UNICODE _UNICODE)
endif()

find_package(webrtc-audio-processing REQUIRED)
find_package(SampleRate REQUIRED)
find_package(dr_libs REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)

add_subdirectory(src)

enable_testing()
find_package(Catch2 QUIET)
if(Catch2_FOUND)
    add_subdirectory(tests)
endif()
