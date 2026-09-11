import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, save

required_conan_version = ">=2.0"

# Релизный тарболл 0.2 собирается autotools и содержит веса модели (src/rnnoise_data.c);
# CMake-сборки в проекте нет, поэтому рецепт кладёт свой CMakeLists.txt по Makefile.am:
# та же библиотека, x86 RTCD (SSE4.1 и AVX2 выбираются по CPUID во время работы).
CMAKELISTS = r"""
cmake_minimum_required(VERSION 3.15)
project(rnnoise C)

set(RNNOISE_SOURCES
    src/denoise.c src/rnn.c src/pitch.c src/kiss_fft.c src/celt_lpc.c src/nnet.c src/nnet_default.c
    src/parse_lpcnet_weights.c src/rnnoise_data.c src/rnnoise_tables.c)
set(RNNOISE_DEFINES RNNOISE_BUILD DISABLE_DEBUG_FLOAT)

option(RNNOISE_X86_RTCD "Runtime CPU detection for SSE4.1/AVX2 kernels" ON)
if(RNNOISE_X86_RTCD AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86_64")
    list(APPEND RNNOISE_SOURCES src/x86/x86_dnn_map.c src/x86/x86cpu.c src/x86/nnet_sse4_1.c src/x86/nnet_avx2.c)
    list(APPEND RNNOISE_DEFINES RNN_ENABLE_X86_RTCD CPU_INFO_BY_C)
    if(MSVC)
        # MSVC не объявляет __SSE2__/__SSE4_1__ (только __AVX2__ при /arch:AVX2). Без __SSE2__
        # vec.h уходит в путь без SIMD и включает os_support.h, которого в тарболле нет; x64
        # всегда с SSE2. Макросы OPUS_X86_MAY_HAVE_* не годятся: celt_lpc.h по ним включает
        # celt_lpc_sse.h из Opus, которого тоже нет.
        list(APPEND RNNOISE_DEFINES __SSE2__)
        set_source_files_properties(src/x86/nnet_sse4_1.c PROPERTIES COMPILE_DEFINITIONS "__SSE4_1__")
        set_source_files_properties(src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/x86/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx;-mfma;-mavx2")
    endif()
endif()

add_library(rnnoise ${RNNOISE_SOURCES})
target_include_directories(rnnoise
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE src)
target_compile_definitions(rnnoise PRIVATE ${RNNOISE_DEFINES})
if(MSVC)
    target_compile_definitions(rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(rnnoise PRIVATE /wd4244 /wd4305 /wd4996)
else()
    target_link_libraries(rnnoise PRIVATE m)
endif()

include(GNUInstallDirs)
install(TARGETS rnnoise
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/rnnoise.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
"""


class RnnoiseConan(ConanFile):
    name = "rnnoise"
    description = "Recurrent neural network for audio noise reduction (48 kHz, 10 ms frames)"
    license = "BSD-3-Clause"
    homepage = "https://github.com/xiph/rnnoise"
    topics = ("audio", "noise-suppression", "rnn")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "x86_rtcd": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "x86_rtcd": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        # Библиотека на C: настройки C++ на package id не влияют.
        self.settings.rm_safe("compiler.cppstd")
        self.settings.rm_safe("compiler.libcxx")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)
        save(self, os.path.join(self.source_folder, "CMakeLists.txt"), CMAKELISTS)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["RNNOISE_X86_RTCD"] = bool(self.options.x86_rtcd)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "COPYING", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "rnnoise")
        self.cpp_info.set_property("cmake_target_name", "rnnoise::rnnoise")
        self.cpp_info.set_property("pkg_config_name", "rnnoise")
        self.cpp_info.libs = ["rnnoise"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["m"]
