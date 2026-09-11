#pragma once

#include <windows.h>

namespace bomboec::tools {

// Имена устройств и пути печатаются в UTF-8: манифест (cmake/utf8.manifest) делает
// UTF-8 кодовой страницей процесса, но не консоли.
inline void useUtf8Console() { SetConsoleOutputCP(CP_UTF8); }

}  // namespace bomboec::tools
