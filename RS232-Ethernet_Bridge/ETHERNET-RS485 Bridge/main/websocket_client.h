



#pragma once
#include "esp_err.h"
#include <stdbool.h>

void initialize_sntp(void);
//static void websocket_send_task(void *pvParameters);

void websocket_reconnect_task(void *pvParameters);

esp_err_t websocket_client_start(const char *session_id, const char *email, const char *password);

// Отправка данных на сервер
esp_err_t websocket_client_send(const char *message);

// Остановка клиента
void websocket_client_stop(void);

bool websocket_send_text(const char *msg);
void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size);