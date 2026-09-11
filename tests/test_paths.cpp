#include "app/paths.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using bomboec::app::dataDirectory;

TEST_CASE("dataDirectory: portable next to the exe, otherwise %LOCALAPPDATA%\\bomboec", "[app]") {
    const fs::path root =
        fs::temp_directory_path() /
        ("bomboec-paths-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path exeDir = root / "exe";
    const fs::path local = root / "local";
    fs::create_directories(exeDir);
    fs::create_directories(local);

    // Ни конфига, ни маркера: каталог в LOCALAPPDATA создаётся.
    CHECK(dataDirectory(exeDir, local) == local / "bomboec");
    CHECK(fs::exists(local / "bomboec"));
    // LOCALAPPDATA неизвестен: рядом с exe.
    CHECK(dataDirectory(exeDir, {}) == exeDir);

    std::ofstream(exeDir / "portable").close();
    CHECK(dataDirectory(exeDir, local) == exeDir);
    fs::remove(exeDir / "portable");
    std::ofstream(exeDir / "bomboec.toml") << "[engine]\n";
    CHECK(dataDirectory(exeDir, local) == exeDir);

    std::error_code ec;
    fs::remove_all(root, ec);
}
