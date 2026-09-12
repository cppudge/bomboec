#pragma once

#include "core/config.h"
#include "engine/engine.h"
#include "engine/watchdog.h"
#include "wasapi/devices.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bomboec::app {

// Движок глазами контроллера: Engine и подделка в тестах.
class IEngine {
public:
    virtual ~IEngine() = default;
    virtual bool start(const AppConfig& cfg, std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual EngineStatus status() const = 0;
    virtual bool reopenReference(std::string& error) = 0;
};

// Система и UI глазами контроллера: лог, уведомления, устройства, часы.
class IHost {
public:
    enum class Level { Info, Warning, Error };

    virtual ~IHost() = default;
    virtual void log(const std::string& line) = 0;
    virtual void notify(const std::string& title, const std::string& text, Level level) = 0;
    // Открыть окно статуса: старт по действию пользователя не удался.
    virtual void showStatus() = 0;
    virtual std::vector<wasapi::DeviceInfo> devices(wasapi::Flow flow) = 0;
    virtual uint64_t nowMs() = 0;
    // Процессы с активной сессией записи на capture endpoint'е (кроме своего);
    // nullopt = узнать нельзя.
    virtual std::optional<std::vector<std::string>> listeners(const std::string& captureId) = 0;
    // Движок запустился или остановился (подсказка иконки).
    virtual void engineStateChanged() = 0;
};

// Политика приложения без Win32: конфиг и файл состояния, старт и остановка движка,
// поиск устройств по имени, watchdog с backoff, повторное открытие reference,
// реакция на уведомления об устройствах, строка статуса в лог раз в минуту, режим on_demand
// (микрофон занят только пока кто-то пишет с микрофона кабеля).
// Трей и окна (main.cpp) только зовут методы и показывают status().
class Controller {
public:
    enum class Slot { Mic, Speakers, Output };

    static constexpr uint64_t kRefRetryMs = 5000;
    static constexpr uint32_t kStatusLogEverySec = 60;

    Controller(IEngine& engine, IHost& host, std::filesystem::path configPath, std::filesystem::path statePath);

    // Конфиг из configPath (создаётся из шаблона), поверх него [devices] из statePath.
    // Предупреждения конфига уходят в лог и одно уведомление.
    bool loadOrCreateConfig(std::string& error);

    // interactive: по действию пользователя (при ошибке окно статуса и уведомление);
    // иначе повтор watchdog'а: уведомление только о первой неудаче серии.
    void start(bool interactive);
    void stop();
    void restartIfRunning();
    // Пункт меню старт/стоп.
    void toggle();
    // Перечитать конфиг и перезапустить; ошибка конфига - уведомление, движок не трогается.
    void reload();
    // Выбор устройства в меню: nullopt = устройство по умолчанию. Сохраняет state и перезапускает.
    void selectDevice(Slot slot, const std::optional<wasapi::DeviceInfo>& device);
    // Раз в секунду.
    void tick();
    // Уведомления об устройствах собраны за окно дебаунса.
    void onDevicesChanged(bool defaultChanged);
    // Без конфига движок не должен стартовать и повторять попытки.
    void disable(const std::string& error);

    const AppConfig& config() const { return cfg_; }
    bool wantRunning() const { return wantRunning_; }
    // Движок остановлен режимом on_demand: с микрофона кабеля никто не пишет.
    bool idle() const { return idle_; }
    // Кто сейчас пишет с микрофона кабеля (для окна статуса).
    const std::vector<std::string>& listeners() const { return listeners_; }
    const std::string& lastError() const { return lastError_; }
    EngineStatus status() const { return engine_.status(); }

    // Строка со счётчиками движка для лога.
    static std::string statusLine(const EngineStatus& s);

private:
    // Если сохранённого id нет среди устройств, ищем по имени и обновляем id; выход без
    // выбора - наш кабель, если установлен.
    void resolveDevicesByName();
    void retryReference(const EngineStatus& s, uint64_t now);
    void updateDemand(uint64_t now);

    IEngine& engine_;
    IHost& host_;
    std::filesystem::path configPath_, statePath_;
    AppConfig cfg_;
    Watchdog watchdog_;
    std::string lastError_;
    bool wantRunning_ = true;
    bool audioLogged_ = false;
    uint64_t refRetryAtMs_ = 0;
    bool refWarned_ = false;
    uint32_t statusLogTicks_ = 0;
    // on_demand: микрофон кабеля (другой конец выхода), пусто = режим неприменим.
    std::string cableMicId_, cableMicName_;
    std::vector<std::string> listeners_;
    bool idle_ = false;
    bool demandUnavailableLogged_ = false;
    uint64_t lastListenerMs_ = 0;
};

}  // namespace bomboec::app
