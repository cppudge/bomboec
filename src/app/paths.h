#pragma once

#include <filesystem>

namespace bomboec::app {

// Каталог данных приложения: конфиг, файл состояния, лог, минидампы.
//  - портативный режим: рядом с exe лежит bomboec.toml или пустой файл-маркер `portable`
//    (сборка разработчика, запуск с флешки) - файлы рядом с exe;
//  - иначе %LOCALAPPDATA%\bomboec (создаётся): под Program Files без повышения прав рядом с
//    exe писать нельзя, первый же запуск не смог бы создать конфиг.
// localAppData пустой - fallback в каталог exe.
std::filesystem::path dataDirectory(const std::filesystem::path& exeDir, const std::filesystem::path& localAppData);

// То же для текущего процесса (каталог exe и %LOCALAPPDATA% из окружения).
std::filesystem::path dataDirectory();

}  // namespace bomboec::app
