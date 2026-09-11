#include "app/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <system_error>

namespace bomboec::app {

namespace fs = std::filesystem;

fs::path dataDirectory(const fs::path& exeDir, const fs::path& localAppData) {
    std::error_code ec;
    if (fs::exists(exeDir / "bomboec.toml", ec) || fs::exists(exeDir / "portable", ec)) return exeDir;
    if (localAppData.empty()) return exeDir;
    const fs::path dir = localAppData / "bomboec";
    fs::create_directories(dir, ec);
    return ec ? exeDir : dir;
}

fs::path dataDirectory() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path exeDir = fs::path(buf).parent_path();
    fs::path localAppData;
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known)) && known) {
        localAppData = known;
    }
    if (known) CoTaskMemFree(known);
    return dataDirectory(exeDir, localAppData);
}

}  // namespace bomboec::app
