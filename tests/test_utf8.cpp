#include "core/utf8.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using bomboec::pathFromUtf8;
using bomboec::pathToUtf8;

TEST_CASE("Process code page is UTF-8 (cmake/utf8.manifest)", "[utf8]") {
    CHECK(GetACP() == CP_UTF8);
    // path(std::string) и path::string() идут через кодовую страницу процесса: без манифеста
    // на системе с ACP 1252 второе бросает исключение, первое даёт мусор.
    const std::string name = "Проба 日本";
    CHECK(std::filesystem::path(name).wstring() == L"Проба 日本");
    CHECK(std::filesystem::path(L"Пётр").string() == "Пётр");
}

TEST_CASE("pathToUtf8 and pathFromUtf8 round-trip regardless of the code page", "[utf8]") {
    const std::filesystem::path p = pathFromUtf8("C:/Записи/日本/take.wav");
    CHECK(p.wstring() == L"C:/Записи/日本/take.wav");
    CHECK(pathToUtf8(p) == "C:/Записи/日本/take.wav");
    CHECK(pathToUtf8(pathFromUtf8("")).empty());
}
