#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bomboec {

// Путь <-> UTF-8 независимо от кодовой страницы процесса. path::string() в MSVC идёт
// через ANSI-страницу и бросает исключение на непредставимых символах, а
// path(std::string) читает байты как ANSI. Конфиг, лог и аргументы утилит в UTF-8.
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

inline std::filesystem::path pathFromUtf8(std::string_view s) {
    return {std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size())};
}

}  // namespace bomboec
