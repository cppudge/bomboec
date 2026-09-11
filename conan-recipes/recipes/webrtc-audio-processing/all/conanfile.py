import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.env import VirtualBuildEnv
from conan.tools.files import copy, get, replace_in_file, rmdir, save
from conan.tools.gnu import PkgConfigDeps
from conan.tools.layout import basic_layout
from conan.tools.meson import Meson, MesonToolchain

required_conan_version = ">=2.0"

FACTORY_H = """// Added by the bomboec Conan recipe: upstream WebRTC ships this factory in
// api/audio/echo_canceller3_factory.h, freedesktop's 2.1 tarball does not.
#ifndef API_AUDIO_ECHO_CANCELLER3_FACTORY_H_
#define API_AUDIO_ECHO_CANCELLER3_FACTORY_H_

#include <memory>
#include <optional>

#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_control.h"
#include "rtc_base/system/rtc_export.h"

namespace webrtc {

class RTC_EXPORT EchoCanceller3Factory : public EchoControlFactory {
 public:
  EchoCanceller3Factory();
  explicit EchoCanceller3Factory(const EchoCanceller3Config& config);
  EchoCanceller3Factory(const EchoCanceller3Config& config,
                        std::optional<EchoCanceller3Config> multichannel_config);

  std::unique_ptr<EchoControl> Create(int sample_rate_hz,
                                      int num_render_channels,
                                      int num_capture_channels) override;

 private:
  const EchoCanceller3Config config_;
  const std::optional<EchoCanceller3Config> multichannel_config_;
};

}  // namespace webrtc

#endif  // API_AUDIO_ECHO_CANCELLER3_FACTORY_H_
"""

FACTORY_CC = """// Added by the bomboec Conan recipe, see echo_canceller3_factory.h.
#include "api/audio/echo_canceller3_factory.h"

#include <memory>

#include "modules/audio_processing/aec3/echo_canceller3.h"

namespace webrtc {

EchoCanceller3Factory::EchoCanceller3Factory() {}

EchoCanceller3Factory::EchoCanceller3Factory(const EchoCanceller3Config& config)
    : config_(config) {}

EchoCanceller3Factory::EchoCanceller3Factory(
    const EchoCanceller3Config& config,
    std::optional<EchoCanceller3Config> multichannel_config)
    : config_(config), multichannel_config_(std::move(multichannel_config)) {}

std::unique_ptr<EchoControl> EchoCanceller3Factory::Create(
    int sample_rate_hz,
    int num_render_channels,
    int num_capture_channels) {
  return std::make_unique<EchoCanceller3>(
      config_, multichannel_config_, sample_rate_hz,
      static_cast<size_t>(num_render_channels),
      static_cast<size_t>(num_capture_channels));
}

}  // namespace webrtc
"""



class WebrtcAudioProcessingConan(ConanFile):
    name = "webrtc-audio-processing"
    description = "AudioProcessing module (AEC3, NS, AGC) extracted from Google's WebRTC"
    license = "BSD-3-Clause"
    homepage = "https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing"
    topics = ("webrtc", "audio", "aec", "aec3", "noise-suppression")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "inline_sse": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "inline_sse": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        basic_layout(self, src_folder="src")

    def requirements(self):
        # Публичные заголовки APM включают absl, поэтому transitive_headers.
        # 2.1 использует absl::Nullable/Nonnull, которых нет в abseil >= 20250512.
        self.requires("abseil/20240722.1", transitive_headers=True, transitive_libs=True)

    def validate(self):
        check_min_cppstd(self, 17)

    def build_requirements(self):
        self.tool_requires("meson/[>=1.4 <2]")
        self.tool_requires("ninja/[>=1.11 <2]")
        if not self.conf.get("tools.gnu:pkg_config", check_type=str):
            self.tool_requires("pkgconf/[>=2.1 <3]")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)
        self._add_echo_canceller3_factory()

    def _add_echo_canceller3_factory(self):
        """Восстанавливает публичную EchoCanceller3Factory, чтобы приложение могло
        передавать EchoCanceller3Config через AudioProcessingBuilder::SetEchoControlFactory."""
        api_dir = os.path.join(self.source_folder, "webrtc", "api")
        save(self, os.path.join(api_dir, "audio", "echo_canceller3_factory.h"), FACTORY_H)
        save(self, os.path.join(api_dir, "audio", "echo_canceller3_factory.cc"), FACTORY_CC)
        # Заголовок в список устанавливаемых api-заголовков.
        nl = chr(10)
        replace_in_file(self, os.path.join(api_dir, "meson.build"),
                        "  ['audio', 'echo_canceller3_config.h'],",
                        "  ['audio', 'echo_canceller3_config.h']," + nl +
                        "  ['audio', 'echo_canceller3_factory.h'],")
        # Реализация компилируется вместе с APM, где доступен echo_canceller3.h.
        apm_meson = os.path.join(self.source_folder, "webrtc", "modules", "audio_processing", "meson.build")
        replace_in_file(self, apm_meson,
                        "webrtc_audio_processing_sources = [" + nl,
                        "webrtc_audio_processing_sources = [" + nl +
                        "  '../../api/audio/echo_canceller3_factory.cc'," + nl)

    def generate(self):
        VirtualBuildEnv(self).generate()
        PkgConfigDeps(self).generate()
        tc = MesonToolchain(self)
        tc.project_options["inline-sse"] = bool(self.options.inline_sse)
        tc.project_options["neon"] = "disabled"
        # abseil строго из Conan, без meson wrap fallback
        tc.project_options["wrap_mode"] = "nofallback"
        tc.generate()

    def build(self):
        meson = Meson(self)
        meson.configure()
        meson.build()

    def package(self):
        copy(self, "COPYING", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        meson = Meson(self)
        meson.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))

    def package_info(self):
        major = self.version.split(".")[0]
        libname = f"webrtc-audio-processing-{major}"
        self.cpp_info.set_property("pkg_config_name", libname)
        self.cpp_info.libs = [libname]
        self.cpp_info.includedirs = [os.path.join("include", libname)]
        self.cpp_info.defines = ["WEBRTC_LIBRARY_IMPL"]
        if self.settings.os == "Windows":
            self.cpp_info.defines += ["WEBRTC_WIN", "NOMINMAX", "_USE_MATH_DEFINES"]
            self.cpp_info.system_libs = ["winmm"]
        elif self.settings.os == "Linux":
            self.cpp_info.defines += ["WEBRTC_POSIX", "WEBRTC_LINUX"]
            self.cpp_info.system_libs = ["pthread", "rt"]
        elif self.settings.os == "Macos":
            self.cpp_info.defines += ["WEBRTC_POSIX", "WEBRTC_MAC"]
