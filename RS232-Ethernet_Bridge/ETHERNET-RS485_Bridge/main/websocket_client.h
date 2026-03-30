



#pragma once
#include "esp_err.h"
#include <stdbool.h>

extern bool ws_connected;          // видимость флага
extern char ws_status_msg[128];    // видимость строки состояния

void initialize_sntp(void);

//static void websocket_send_task(void *pvParameters);

void websocket_reconnect_task(void *pvParameters);

esp_err_t websocket_client_start(const char *session_id, const char *email, const char *password , const char *node_name );
void websocket_restart(const char *email, const char *password , const char *node_name );

// Отправка данных на сервер
esp_err_t websocket_client_send(const char *message);

// Остановка клиента
void websocket_client_stop(void);
void get_time_iso(char *buf, size_t len);
bool websocket_send_text(const char *msg);
void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size);
int hex_to_bytes(const char *in, uint8_t *out, int max_len);
void websocket_disable_reconnect(void);
void websocket_enable_reconnect(void);
