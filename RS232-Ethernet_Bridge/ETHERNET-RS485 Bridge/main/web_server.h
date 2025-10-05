



#pragma once

#include "esp_err.h"

// Инициализация HTTP сервера (отдаёт файлы из LittleFS)
esp_err_t web_server_start(void);

// Остановка сервера
esp_err_t web_server_stop(void);

