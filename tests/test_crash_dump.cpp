#include "app/crash_dump.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_CASE("writeMiniDump writes a dump of the running process", "[crash]") {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / ("bomboec-test-" + std::to_string(GetCurrentProcessId()) + ".dmp");
    REQUIRE(bomboec::crash::writeMiniDump(file, nullptr));
    CHECK(std::filesystem::file_size(file) > 4096);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
