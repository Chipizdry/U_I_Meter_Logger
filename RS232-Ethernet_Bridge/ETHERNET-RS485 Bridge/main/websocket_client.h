



#pragma once
#include "esp_err.h"
void initialize_sntp(void);
static void websocket_send_task(void *pvParameters);

// Инициализация и запуск клиента
//esp_err_t websocket_client_start(const char *uri);

esp_err_t websocket_client_start(const char *session_id, const char *email, const char *password);

// Отправка данных на сервер
esp_err_t websocket_client_send(const char *message);

// Остановка клиента
void websocket_client_stop(void);


