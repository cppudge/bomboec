#pragma once

#include <windows.h>

#include <filesystem>

namespace bomboec::crash {

// Минидамп при падении: необработанное SEH-исключение, std::terminate (исключение
// из потока, join забытого std::thread), abort, pure virtual call, недопустимый
// параметр CRT. Дамп пишется в dumpDir как bomboec-<дата>-<время>-<pid>.dmp и
// открывается в Visual Studio или WinDbg вместе с bomboec.pdb той же сборки.
void install(const std::filesystem::path& dumpDir);

// Дамп текущего процесса в file; ep может быть nullptr. Для обработчика и тестов.
bool writeMiniDump(const std::filesystem::path& file, EXCEPTION_POINTERS* ep);

}  // namespace bomboec::crash
