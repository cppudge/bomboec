cmake_minimum_required(VERSION 3.28)

# ===== Conan (cmake-conan provider) =====
# При обычной настройке через CMake провайдер сам запускает `conan install` на первом
# find_package. Внутри `conan build` (conanfile.py выставляет SKIP_CONAN_PROVIDER_CMAKE)
# блок пропускается.
if(NOT DEFINED SKIP_CONAN_PROVIDER_CMAKE)
    # Домашняя папка Conan в корне проекта (./.conan2): remotes и кэш пакетов не зависят от
    # глобального конфига пользователя (корпоративные remotes, global.conf). Явный CONAN_HOME
    # в окружении важнее.
    option(BOMBOEC_LOCAL_CONAN_HOME "Use a project-local .conan2 Conan home" ON)
    if(BOMBOEC_LOCAL_CONAN_HOME AND NOT DEFINED ENV{CONAN_HOME})
        set(ENV{CONAN_HOME} "${CMAKE_SOURCE_DIR}/.conan2")
        # Локальный индекс рецептов (webrtc-audio-processing и другие, которых нет в Conan
        # Center) первым, чтобы он был важнее conancenter. --force делает вызов идемпотентным.
        find_program(CONAN_COMMAND conan REQUIRED)
        execute_process(
            COMMAND "${CONAN_COMMAND}" remote add --force --index 0 bomboec-local
                    "${CMAKE_SOURCE_DIR}/conan-recipes" --type local-recipes-index
            OUTPUT_QUIET
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
    # Профили целиком выводятся из компилятора и настроек, которые нашёл CMake.
    set(CONAN_HOST_PROFILE "auto-cmake" CACHE STRING "Conan host profile")
    set(CONAN_BUILD_PROFILE "auto-cmake" CACHE STRING "Conan build profile")
    set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES "${CMAKE_SOURCE_DIR}/cmake/cmake-conan/conan_provider.cmake")
endif()

project(bomboec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # для clangd в IDE
# Все exe в одном месте: приложение, утилиты и тесты.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# Код чист на /W4, поэтому предупреждения по умолчанию ошибки. -DBOMBOEC_WERROR=OFF, если новый
# компилятор найдёт что-то раньше, чем это успеют поправить.
option(BOMBOEC_WERROR "Treat compiler warnings as errors" ON)

# Общие флаги и макросы для всех наших целей: каждая линкует bomboec::options (PRIVATE).
add_library(bomboec_options INTERFACE)
add_library(bomboec::options ALIAS bomboec_options)
if(MSVC)
    target_compile_options(bomboec_options INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus $<$<BOOL:${BOMBOEC_WERROR}>:/WX>)
    target_compile_definitions(bomboec_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

find_package(webrtc-audio-processing REQUIRED)
find_package(dr_libs REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(cxxopts REQUIRED)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests)
endif()
