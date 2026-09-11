#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace bomboec {

// Когда перезапускать движок. Чистая логика: монотонное время (мс) и наблюдения
// приходят снаружи раз в секунду, наружу уходит решение, перезапуск делает
// приложение.
//  - поток сообщил ошибку (устройство пропало) -> Restart;
//  - движок работает, но кадры не идут kStallMs (микрофон молчит без ошибки) -> Restart;
//  - движок не работает, хотя должен (старт не удался, устройства ещё нет) -> Start,
//    между попытками backoff 1, 2, 5, 10, 30 с;
//  - после kHealthyMs нормальной работы счётчик неудач обнуляется.
class Watchdog {
public:
    enum class Action { None, Start, Restart };
    enum class Reason { None, ThreadError, Stall };

    static constexpr uint64_t kStallMs = 3000;
    static constexpr uint64_t kHealthyMs = 30'000;

    // Движок запущен (пользователем или по Action::Start).
    void onStarted(uint64_t nowMs) {
        lastFrames_ = 0;
        lastProgressMs_ = nowMs;
        startedMs_ = nowMs;
    }

    // Старт не удался или движок остановлен по Action::Restart: следующая попытка через backoff.
    void onFailed(uint64_t nowMs) {
        static constexpr std::array<uint64_t, 5> kBackoffMs{1000, 2000, 5000, 10'000, 30'000};
        nextAttemptMs_ = nowMs + kBackoffMs[std::min<size_t>(failures_, kBackoffMs.size() - 1)];
        ++failures_;
    }

    // Пользователь сам запустил, остановил или перенастроил движок: прошлые неудачи не в счёт.
    void clear() {
        failures_ = 0;
        nextAttemptMs_ = 0;
        reason_ = Reason::None;
    }

    // Устройство появилось (уведомление системы): следующая попытка старта сразу, backoff не ждём.
    void retryNow() { nextAttemptMs_ = 0; }

    // running, threadError, framesProcessed: из EngineStatus. Вызывать, только пока
    // пользователь хочет, чтобы движок работал.
    Action tick(uint64_t nowMs, bool running, bool threadError, uint64_t framesProcessed) {
        if (!running) return nowMs >= nextAttemptMs_ ? Action::Start : Action::None;
        if (threadError) {
            reason_ = Reason::ThreadError;
            return Action::Restart;
        }
        if (framesProcessed != lastFrames_) {
            lastFrames_ = framesProcessed;
            lastProgressMs_ = nowMs;
            if (failures_ > 0 && nowMs - startedMs_ >= kHealthyMs) failures_ = 0;
            return Action::None;
        }
        if (nowMs - lastProgressMs_ >= kStallMs) {
            reason_ = Reason::Stall;
            return Action::Restart;
        }
        return Action::None;
    }

    Reason reason() const { return reason_; }  // причина последнего Restart
    uint32_t failures() const { return failures_; }
    uint64_t nextAttemptMs() const { return nextAttemptMs_; }

private:
    uint32_t failures_ = 0;
    uint64_t nextAttemptMs_ = 0;
    uint64_t lastFrames_ = 0;
    uint64_t lastProgressMs_ = 0;
    uint64_t startedMs_ = 0;
    Reason reason_ = Reason::None;
};

// Что делать по уведомлению системы об аудиоустройствах (IMMNotificationClient),
// когда события за окно дебаунса собраны. Чистая логика, факты собирает приложение.
struct DeviceChangeFacts {
    bool running = false;
    bool wantRunning = true;
    bool micPresent = true;       // микрофон движка всё ещё среди активных endpoint'ов
    bool outputPresent = true;    // выход движка тоже
    bool referenceActive = true;  // loopback колонок работает
    bool defaultChanged = false;  // сменилось устройство по умолчанию
    bool usesDefault = false;     // хотя бы одно устройство в конфиге = "по умолчанию" (пустой id)
};

enum class DeviceChangeAction {
    None,
    Restart,          // устройство движка пропало или сменился нужный default: стоп и старт заново
    ReopenReference,  // движок работает без reference: попробовать открыть колонки сейчас
    StartNow,         // движок должен работать, но стоит: попытка старта без ожидания backoff
};

inline DeviceChangeAction decideDeviceChange(const DeviceChangeFacts& f) {
    if (!f.wantRunning) return DeviceChangeAction::None;
    if (!f.running) return DeviceChangeAction::StartNow;
    if (!f.micPresent || !f.outputPresent) return DeviceChangeAction::Restart;
    if (f.defaultChanged && f.usesDefault) return DeviceChangeAction::Restart;
    if (!f.referenceActive) return DeviceChangeAction::ReopenReference;
    return DeviceChangeAction::None;
}

}  // namespace bomboec
