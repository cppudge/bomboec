#include "wasapi/devices.h"

#include <catch2/catch_test_macros.hpp>

using bomboec::wasapi::DeviceInfo;
using bomboec::wasapi::sameVirtualDevice;

TEST_CASE("sameVirtualDevice catches both ends of a virtual cable only", "[devices]") {
    // Значения свойств как у реальных endpoint'ов (bomboec Cable, Steam, USB-микрофон).
    const DeviceInfo cableMic{L"{c1}", L"Microphone (bomboec Cable)", L"bomboec Cable", L"ROOT"};
    const DeviceInfo cableSpk{L"{c2}", L"Speakers (bomboec Cable)", L"bomboec Cable", L"ROOT"};
    const DeviceInfo steam{L"{s}", L"Speakers (Steam Streaming Speakers)", L"Steam Streaming Speakers", L"ROOT"};
    const DeviceInfo usbMic{L"{y1}", L"Microphone (Yeti Orb)", L"Yeti Orb", L"USB"};
    const DeviceInfo usbPhones{L"{y2}", L"Headphones (Yeti Orb)", L"Yeti Orb", L"USB"};
    const DeviceInfo unknown{L"{u}", L"?", L"", L"ROOT"};

    CHECK(sameVirtualDevice(cableMic, cableSpk));
    CHECK_FALSE(sameVirtualDevice(cableMic, steam));
    CHECK_FALSE(sameVirtualDevice(usbMic, usbPhones));  // гарнитура: связь только акустическая
    CHECK_FALSE(sameVirtualDevice(unknown, unknown));   // без имени адаптера не решаем
}
