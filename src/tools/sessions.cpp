// bomboec-sessions: кто держит открытыми аудиопотоки. Для каждого активного endpoint'а
// (или только тех, чьё имя содержит --filter) печатает процессы с активной сессией.
// Так проверяется, слушает ли кто-то микрофон кабеля (режим on_demand) и кто мешает
// переустановке драйвера (install.ps1). Код возврата 1, если такие процессы есть.

#include "wasapi/sessions.h"
#include "console.h"
#include "wasapi/com_util.h"
#include "wasapi/devices.h"

#include <cxxopts.hpp>

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace ws = bomboec::wasapi;

namespace {

int run(int argc, char** argv) {
    cxxopts::Options options("bomboec-sessions", "processes with active audio sessions per endpoint");
    // clang-format off
    options.add_options()
        ("f,filter", "only endpoints whose name contains this", cxxopts::value<std::string>())
        ("a,all", "list endpoints without active sessions too")
        ("h,help", "help");
    // clang-format on
    const cxxopts::ParseResult args = options.parse(argc, argv);
    if (args.contains("help")) {
        std::printf("%s\n", options.help().c_str());
        return 0;
    }
    const std::string filter = args.contains("filter") ? args["filter"].as<std::string>() : "";
    const bool all = args.contains("all");

    const ws::ComInit com;
    int busy = 0;
    for (const ws::Flow flow : {ws::Flow::Capture, ws::Flow::Render}) {
        const char* flowName = flow == ws::Flow::Capture ? "capture" : "render";
        std::string error;
        for (const ws::DeviceInfo& d : ws::enumerateDevices(flow, error)) {
            const std::string name = ws::toUtf8(d.name);
            if (!filter.empty() && name.find(filter) == std::string::npos) continue;
            const ws::ComPtr<IMMDevice> dev = ws::openDevice(flow, d.id, error);
            std::vector<std::string> names;
            if (!dev || !ws::activeSessionProcesses(dev.Get(), 0, names, error)) {
                std::printf("%s %s: error: %s\n", flowName, name.c_str(), error.c_str());
                continue;
            }
            if (names.empty() && !all) continue;
            std::string list;
            for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
            std::printf("%s %s: %s\n", flowName, name.c_str(), names.empty() ? "-" : list.c_str());
            busy += names.empty() ? 0 : 1;
        }
        if (!error.empty()) std::printf("%s\n", error.c_str());
    }
    return busy > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bomboec::tools::useUtf8Console();
    // Исключения (разбор аргументов cxxopts, std): сообщение вместо abort.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
