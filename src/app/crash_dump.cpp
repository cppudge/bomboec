#include "app/crash_dump.h"

#include <dbghelp.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>

namespace bomboec::crash {

namespace {

// Своё SEH-исключение: terminate, abort и ошибки CRT сводятся к одному фильтру,
// который получает контекст падения и пишет дамп.
constexpr DWORD kFatalCode = 0xE0B0EC01;

// Заполняется в install(): в момент падения куча может быть испорчена, поэтому
// путь к дампу собирается без выделения памяти.
wchar_t gDumpDir[MAX_PATH] = L"";

bool writeDump(const wchar_t* file, EXCEPTION_POINTERS* ep) {
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    const auto type =
        MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
    const BOOL ok =
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type, ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* ep) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t file[MAX_PATH + 64];
    swprintf_s(file, L"%s\\bomboec-%04u%02u%02u-%02u%02u%02u-%lu.dmp", gDumpDir, st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, GetCurrentProcessId());
    writeDump(file, ep);
    return EXCEPTION_EXECUTE_HANDLER;  // процесс завершается
}

[[noreturn]] void raiseFatal() {
    RaiseException(kFatalCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::_Exit(3);  // сюда не доходит: фильтр завершает процесс
}

// abort() и так завершает процесс; дамп важнее строгой async-signal-safety.
void onAbortSignal(int /*sig*/) {
    raiseFatal();  // NOLINT(bugprone-signal-handler)
}

void onPurecall() { raiseFatal(); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { raiseFatal(); }

}  // namespace

bool writeMiniDump(const std::filesystem::path& file, EXCEPTION_POINTERS* ep) { return writeDump(file.c_str(), ep); }

void install(const std::filesystem::path& dumpDir) {
    wcsncpy_s(gDumpDir, dumpDir.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(onUnhandledException);
    std::set_terminate([] { raiseFatal(); });
    // Без _CALL_REPORTFAULT abort() идёт в SIGABRT, а не в __fastfail мимо фильтра.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, onAbortSignal);
    _set_purecall_handler(onPurecall);
    _set_invalid_parameter_handler(onInvalidParameter);
}

}  // namespace bomboec::crash
