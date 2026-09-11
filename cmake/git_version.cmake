# Пишет version.h: версия проекта и git describe. Запускается на каждой сборке
# (src/app/CMakeLists.txt); configure_file перезаписывает файл только при изменении,
# поэтому лишней пересборки нет.
#   cmake -DSOURCE_DIR=<repo> -DINPUT=<version.h.in> -DOUTPUT=<version.h> -DVERSION=<x.y.z> -P git_version.cmake
execute_process(
    COMMAND git describe --always --dirty --tags --abbrev=8
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE BOMBOEC_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_result)
if(NOT git_result EQUAL 0 OR BOMBOEC_GIT_DESCRIBE STREQUAL "")
    set(BOMBOEC_GIT_DESCRIBE "unknown")
endif()
set(BOMBOEC_VERSION "${VERSION}")
configure_file("${INPUT}" "${OUTPUT}" @ONLY)
