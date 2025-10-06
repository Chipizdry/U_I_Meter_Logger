



#pragma once
#include "esp_err.h"

// Инициализация и запуск клиента
esp_err_t websocket_client_start(const char *uri);

// Отправка данных на сервер
esp_err_t websocket_client_send(const char *message);

// Остановка клиента
void websocket_client_stop(void);


