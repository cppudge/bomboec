from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class BomboecConan(ConanFile):
    name = "bomboec"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    def requirements(self):
        self.requires("webrtc-audio-processing/2.1")
        self.requires("dr_libs/cci.20230529")
        self.requires("tomlplusplus/3.4.0")
        self.requires("cxxopts/3.3.1")
        self.test_requires("catch2/3.16.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
