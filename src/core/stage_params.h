#pragma once

#include <toml++/toml.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace bomboec {

// Параметры стадии из [[chain]]: типизированные чтения с проверкой типа и диапазона
// плюс учёт запрошенных ключей. Стадия в init() читает все свои ключи, затем Chain
// спрашивает unknownKeys(): опечатка (filter_lenght_blocks) не остаётся молча
// значением по умолчанию, а неверный тип (aec = "yes") - ошибка, а не true.
//
// Чтение при ошибке возвращает значение по умолчанию и запоминает текст в error();
// стадия проверяет ok() один раз в конце. Это же будущая граница для стадий-плагинов:
// только строки, числа и булевы значения, без toml++ в интерфейсе.
class StageParams {
public:
    StageParams() = default;
    explicit StageParams(const toml::table& table) : table_(&table) {}

    bool boolean(std::string_view key, bool defaultValue);
    int64_t integer(std::string_view key, int64_t defaultValue, int64_t lo, int64_t hi);
    double number(std::string_view key, double defaultValue, double lo, double hi);
    std::string string(std::string_view key, std::string_view defaultValue);
    // Строка из списка допустимых.
    std::string choice(std::string_view key, std::string_view defaultValue,
                       std::initializer_list<std::string_view> allowed);

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Ключи таблицы, которые стадия не запрашивала (после init()).
    std::vector<std::string> unknownKeys() const;

private:
    const toml::node* fetch(std::string_view key);
    void fail(std::string_view key, const std::string& what);

    const toml::table* table_ = nullptr;
    std::vector<std::string> used_;
    std::string error_;
};

}  // namespace bomboec
