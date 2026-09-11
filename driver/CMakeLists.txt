# bomboec_cable.sys: kernel-mode WaveRT virtual audio cable (portcls, KMDF miniport).
#
# Отдельный CMake-проект: собирается напрямую cl/link из окружения MSVC с флагами
# kernel-mode, без расширения WDK для Visual Studio. См. driver/build.ps1.

cmake_minimum_required(VERSION 3.28)
project(bomboec_cable LANGUAGES CXX RC)

set(WDK_ROOT "C:/Program Files (x86)/Windows Kits/10" CACHE PATH "Windows Kits root")
set(WDK_VERSION "10.0.26100.0" CACHE STRING "WDK/SDK version")
set(KMDF_VERSION "1.33" CACHE STRING "KMDF version")

set(WDK_INC "${WDK_ROOT}/Include/${WDK_VERSION}")
set(WDK_LIB "${WDK_ROOT}/Lib/${WDK_VERSION}")
set(KMDF_INC "${WDK_ROOT}/Include/wdf/kmdf/${KMDF_VERSION}")
set(KMDF_LIB "${WDK_ROOT}/Lib/wdf/kmdf/x64/${KMDF_VERSION}")

if(NOT EXISTS "${WDK_INC}/km/ntddk.h")
    message(FATAL_ERROR "WDK kernel headers not found: ${WDK_INC}/km")
endif()

# Сбрасываем user-mode умолчания CMake для MSVC.
set(CMAKE_CXX_FLAGS "")
set(CMAKE_CXX_FLAGS_DEBUG "")
set(CMAKE_CXX_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "")
set(CMAKE_CXX_FLAGS_MINSIZEREL "")
set(CMAKE_EXE_LINKER_FLAGS "")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "")
set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "")
set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES
    src/adapter.cpp
    src/basetopo.cpp
    src/cable.cpp
    src/common.cpp
    src/hw.cpp
    src/kshelper.cpp
    src/micarraytopo.cpp
    src/mintopo.cpp
    src/minwavert.cpp
    src/minwavertstream.cpp
    src/NewDelete.cpp
    src/speakertopo.cpp
    src/bomboec_cable.rc
)

add_executable(bomboec_cable ${SOURCES})
set_target_properties(bomboec_cable PROPERTIES SUFFIX ".sys")

target_include_directories(bomboec_cable PRIVATE
    src
    "${WDK_INC}/km"
    "${WDK_INC}/km/crt"
    "${WDK_INC}/shared"
    "${KMDF_INC}"
)
# Ресурсы компилируются user-mode заголовками (windows.h, ntverp.h, common.ver).
set_source_files_properties(src/bomboec_cable.rc PROPERTIES
    INCLUDE_DIRECTORIES "${WDK_INC}/um;${WDK_INC}/shared")

target_compile_definitions(bomboec_cable PRIVATE
    _WIN64 _AMD64_ AMD64
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 WINNT=1 NTDDI_VERSION=0x0A00000C
    DEPRECATE_DDK_FUNCTIONS=1 POOL_NX_OPTIN=1
    KMDF_VERSION_MAJOR=1 KMDF_VERSION_MINOR=33
    _WIN32 UNICODE _UNICODE PC_IMPLEMENTATION DEBUG_LEVEL=DEBUGLVL_TERSE _NEW_DELETE_OPERATORS_
)

target_compile_options(bomboec_cable PRIVATE
    /kernel /W4 /WX- /GS /Gy /Gm- /Zi /O2 /Oi
    /EHs-c- /GR- /Zc:threadSafeInit- /Zc:wchar_t
    /wd4603 /wd4627 /wd4986 /wd4987 /wd4748 /wd4201 /wd4100 /wd4996 /wd4324
    -cbstring /d1import_no_registry
)

target_link_options(bomboec_cable PRIVATE
    /DRIVER /SUBSYSTEM:NATIVE,10.0 /ENTRY:FxDriverEntry
    /NODEFAULTLIB /INCREMENTAL:NO /RELEASE /MANIFEST:NO
    /OPT:REF /OPT:ICF /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /SECTION:INIT,d
    /DYNAMICBASE /NXCOMPAT /DEBUG /osversion:10.0 /pdbcompress /debugtype:pdata
    /kernel /IGNORE:4078,4221
)

target_link_libraries(bomboec_cable PRIVATE
    "${WDK_LIB}/km/x64/BufferOverflowFastFailK.lib"
    "${WDK_LIB}/km/x64/ntoskrnl.lib"
    "${WDK_LIB}/km/x64/hal.lib"
    "${WDK_LIB}/km/x64/wmilib.lib"
    "${KMDF_LIB}/WdfLdr.lib"
    "${KMDF_LIB}/WdfDriverEntry.lib"
    "${WDK_LIB}/km/x64/portcls.lib"
    "${WDK_LIB}/km/x64/stdunk.lib"
    "${WDK_LIB}/km/x64/libcntpr.lib"
    "${WDK_LIB}/km/x64/ksguid.lib"
)

# Пакет: .sys + .inf рядом, дальше inf2cat/signtool в build.ps1.
add_custom_command(TARGET bomboec_cable POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/package"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:bomboec_cable>" "${CMAKE_BINARY_DIR}/package/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/bomboec_cable.inf" "${CMAKE_BINARY_DIR}/package/"
)
