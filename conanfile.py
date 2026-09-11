from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class BomboecConan(ConanFile):
    name = "bomboec"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("webrtc-audio-processing/2.1")
        self.requires("rnnoise/0.2")
        self.requires("dr_libs/cci.20230529")
        self.requires("tomlplusplus/3.4.0")
        self.requires("cxxopts/3.3.1")
        self.test_requires("catch2/3.16.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        # CMake запускается через CMakePresets.json и cmake-conan provider: Conan не должен
        # дописывать свой CMakeUserPresets.json (с несколькими build-папками он копил бы
        # include'ы с одинаковыми именами пресетов).
        tc.user_presets_path = False
        # Внутри `conan build` провайдер из CMakeLists.txt запускаться не должен.
        tc.cache_variables["SKIP_CONAN_PROVIDER_CMAKE"] = True
        tc.generate()
