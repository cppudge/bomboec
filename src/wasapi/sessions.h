#pragma once

#include "wasapi/com_util.h"

#include <mmdeviceapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bomboec::wasapi {

// Процессы с активным аудиопотоком на endpoint'е (IAudioSessionManager2 видит сессии всех
// процессов). Для микрофона кабеля это ответ на вопрос «слушает ли кто-то виртуальный
// микрофон»: по нему приложение занимает физический микрофон только на время звонка.
// Процесс excludePid (обычно свой) не считается. Имя процесса, если его не удалось узнать
// (чужой пользователь, повышенные права), заменяется на "pid N".
bool activeSessionProcesses(IMMDevice* device, uint32_t excludePid, std::vector<std::string>& names,
                            std::string& error);

}  // namespace bomboec::wasapi
