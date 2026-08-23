





#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_https_ota.h"
#include "lwip/netdb.h"
#include "driver/uart.h"
#include "cJSON.h"


#include "websocket_events.h"
#include "nvs_settings.h"
#include "rs485_master.h"
#include "ws_server.h"
#include "gpio_manager.h"
#include "web_server.h"
#include "ota_pull.h"

 TickType_t last_ws_event_tick = 0; // последняя активность WebSocket

static const TickType_t WS_TIMEOUT_TICKS = pdMS_TO_TICKS(20000); // 20 секунд
// переменные для авторизации
 char ws_email[64];
 char ws_password[64];
 char ws_node_name[64];
 char ws_session_id[128] = {0};

static bool ws_reconnect_in_progress = false;
extern bool test_account_active;// из ws_server.c 
extern void ws_broadcast(const char *text);
extern httpd_handle_t server;

extern esp_err_t rs485_master_send(const uint8_t *data, size_t len);
// Встроенный сертификат (объявляется линковщиком)
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

int hex_to_bytes(const char *in, uint8_t *out, int max_len);
void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size);
void get_time_iso(char *buf, size_t len);

static const char *TAG = "websocket_client";
 esp_websocket_client_handle_t client = NULL;

bool ws_connected = false;
static bool ws_reconnect_enabled = true;
static int reconnect_fail_count = 0;
static int reconnect_delay_sec = 5;        // начальная задержка 5 сек
static const int MAX_RECONNECT_DELAY_SEC = 120;   // максимум 2 минуты
static const int MAX_DNS_ERRORS = 10;               // после 10 DNS-ошибок отключаем реконнект

char cloud_status_msg[32] = "Idle";   // статус по умолчанию
char ws_rx_buf[512];
static int ws_rx_len = 0;


void websocket_reconnect_task(void *pvParameters);

void websocket_disable_reconnect(void)
{
    ws_reconnect_enabled = false;
    ws_reconnect_in_progress = false;
}

void websocket_enable_reconnect(void)
{
    ws_reconnect_enabled = true;
}


static bool can_resolve_host(const char *host)
{
    struct hostent *he = gethostbyname(host);
    return (he != NULL);
}



/*
 void websocket_reconnect_task(void *pvParameters)
{
    const TickType_t check_interval = pdMS_TO_TICKS(5000); // проверяем каждую секунду

    for (;;)
    {

        if (!ws_reconnect_enabled) {
            vTaskDelay(check_interval);
            continue;     // реконс не работает
        }
        bool need_reconnect = false;

        if (!ws_connected) {
            need_reconnect = true;
        } else {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ws_event_tick) > WS_TIMEOUT_TICKS) {
                ESP_LOGW(TAG, "⚠️ No WS activity for 20 seconds, forcing reconnect");
                need_reconnect = true;
            }
        }

        if (need_reconnect) {
    if (ws_reconnect_in_progress) {
        vTaskDelay(check_interval);
        continue;
    }

    ws_reconnect_in_progress = true;

    if (client) {
        esp_websocket_client_stop(client);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_destroy(client);
        client = NULL;
        ws_connected = false;
    }

    const char *email_to_use = test_account_active ? ws_email : user_cfg.account_login;
    const char *pass_to_use  = test_account_active ? ws_password : user_cfg.account_password;
    const char *node_name_use  = test_account_active ? ws_node_name : user_cfg.node_name;


    esp_err_t ok = websocket_client_start(user_cfg.serial, email_to_use, pass_to_use, node_name_use);
    if (ok == ESP_OK) {
        ESP_LOGI(TAG, "Reconnect started");
    } else {
        ESP_LOGE(TAG, "Reconnect failed: %s", esp_err_to_name(ok));
    }

    ws_reconnect_in_progress = false;
}

        vTaskDelay(check_interval);
    }
}

*/

void websocket_reconnect_task(void *pvParameters)
{
    const TickType_t base_delay_ticks = pdMS_TO_TICKS(5000); // для проверки флага

    for (;;)
    {
        if (!ws_reconnect_enabled) {
            vTaskDelay(base_delay_ticks);
            continue;
        }

        bool need_reconnect = false;
        if (!ws_connected) {
            need_reconnect = true;
        } else {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ws_event_tick) > WS_TIMEOUT_TICKS) {
                ESP_LOGW(TAG, "⚠️ No WS activity for 20 seconds, forcing reconnect");
                need_reconnect = true;
            }
        }

        if (need_reconnect && !ws_reconnect_in_progress) {
            ws_reconnect_in_progress = true;

            // Останавливаем и уничтожаем старый клиент, если есть
            if (client) {
                esp_websocket_client_stop(client);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_websocket_client_destroy(client);
                client = NULL;
                ws_connected = false;
            }

            // Проверяем, можно ли резолвить хост (извлечём хост из URI)
            // Для простоты – проверяем фиксированный домен (или можно парсить из sys.ws_server)
            if (!can_resolve_host("dev.monitoring.cor-int.com")) {
                ESP_LOGE(TAG, "❌ Cannot resolve host, postponing reconnect");
                reconnect_fail_count++;
                if (reconnect_fail_count >= MAX_DNS_ERRORS) {
                    ESP_LOGE(TAG, "Too many DNS errors, disabling reconnect");
                    websocket_disable_reconnect();
                    ws_reconnect_in_progress = false;
                    break;  // выходим из задачи (или можно просто ждать вечно)
                }
                // Увеличиваем задержку
                reconnect_delay_sec = (reconnect_delay_sec * 2 < MAX_RECONNECT_DELAY_SEC) ? reconnect_delay_sec * 2 : MAX_RECONNECT_DELAY_SEC;
                ESP_LOGI(TAG, "Next reconnect attempt in %d seconds", reconnect_delay_sec);
                vTaskDelay(pdMS_TO_TICKS(reconnect_delay_sec * 1000));
                ws_reconnect_in_progress = false;
                continue;
            }

            // Сброс счётчика ошибок при успешном резолвинге
            reconnect_fail_count = 0;

            const char *email_to_use = test_account_active ? ws_email : user_cfg.account_login;
            const char *pass_to_use  = test_account_active ? ws_password : user_cfg.account_password;
            const char *node_name_use  = test_account_active ? ws_node_name : user_cfg.node_name;

            esp_err_t ok = websocket_client_start(user_cfg.serial, email_to_use, pass_to_use, node_name_use);
            if (ok == ESP_OK) {
                ESP_LOGI(TAG, "Reconnect started");
                reconnect_delay_sec = 5; // сбрасываем задержку при успехе
            } else {
                ESP_LOGE(TAG, "Reconnect failed: %s", esp_err_to_name(ok));
                reconnect_fail_count++;
                // Увеличиваем задержку
                reconnect_delay_sec = (reconnect_delay_sec * 2 < MAX_RECONNECT_DELAY_SEC) ? reconnect_delay_sec * 2 : MAX_RECONNECT_DELAY_SEC;
                ESP_LOGI(TAG, "Next reconnect attempt in %d seconds", reconnect_delay_sec);
                vTaskDelay(pdMS_TO_TICKS(reconnect_delay_sec * 1000));
                ws_reconnect_in_progress = false;
                continue;
            }

            ws_reconnect_in_progress = false;
        }

        // Ждём перед следующей проверкой (но не блокируем надолго, чтобы отреагировать на изменения)
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}



static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
        case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "✅ Connecting to WebSocket server...");
       
        last_ws_event_tick = xTaskGetTickCount(); // сброс таймера при подключении
            // отправляем авторизационное сообщение
            if (strlen(ws_email) > 0 && strlen(ws_password) > 0) {
                char auth_msg[256];
                snprintf(auth_msg, sizeof(auth_msg), "{\"email\":\"%s\", \"password\":\"%s\", \"node_name\":\"%s\"}", ws_email, ws_password, ws_node_name);

                esp_websocket_client_send_text(client, auth_msg, strlen(auth_msg), portMAX_DELAY);
                ESP_LOGI(TAG, "📤Sent auth message:%s", auth_msg);
            }
            ws_connected = true;
           //  gpio_set_net_led(true);
             ws_broadcast("{\"cloud_status\":\"Connecting...\"}");
             last_ws_event_tick = xTaskGetTickCount(); // фиксируем активность
             ws_reconnect_in_progress = false; // сбрасываем флаг реконнекта
        break;


        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️ WebSocket disconnected!");
            ws_connected = false;
             gpio_set_net_led(false);
            ws_broadcast("{\"cloud_status\":\"disconnected\"}");
        break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket error!");
            ws_connected = false;
             gpio_set_net_led(false);
            ws_broadcast("{\"cloud_status\":\"error\"}");
        break;

        case WEBSOCKET_EVENT_DATA:
            last_ws_event_tick = xTaskGetTickCount();
            if (!data->data_ptr || data->data_len <= 0) return;

            // ─────── Разделение по типу ───────
            if (data->op_code == 2)  {
                // Heartbeat / keepalive пакеты
                ESP_LOGI(TAG, "💓 Heartbeat/Binary received, len=%d", data->data_len);
                ESP_LOG_BUFFER_HEX(TAG, data->data_ptr, data->data_len);
                break;  // не парсим дальше
            }

            if (data->op_code == 1) {
                // Текстовый пакет — собираем буфер на случай фрагментации
                if (ws_rx_len + data->data_len < sizeof(ws_rx_buf) - 1) {
                    memcpy(ws_rx_buf + ws_rx_len, data->data_ptr, data->data_len);
                    ws_rx_len += data->data_len;
                    ws_rx_buf[ws_rx_len] = 0;
                } else {
                    ESP_LOGW(TAG, "⚠️ WS buffer overflow, discarding data");
                    ws_rx_len = 0;
                    ws_rx_buf[0] = 0;
                    break;
                }

                // Если пакет ещё фрагментирован, ждём следующую часть
                if (!data->fin) return;

                // ─────── Обработка собранного текста ───────
                ESP_LOGI(TAG, "📝 WS Text: %s", ws_rx_buf);

                 if (strcmp(ws_rx_buf, "ping") == 0) {
                    ESP_LOGI(TAG, "Ping received → sending Pong");

                    if (client && esp_websocket_client_is_connected(client)) {
                        esp_websocket_client_send_text(client, "pong", 4, 1000 / portTICK_PERIOD_MS);
                    }

                    // очищаем буфер и ВЫХОДИМ (важно!)
                    ws_rx_len = 0;
                    ws_rx_buf[0] = 0;
                    return;
                }
                websocket_process_message(ws_rx_buf);
                // Очистка буфера
                ws_rx_len = 0;
                ws_rx_buf[0] = 0;
            }
        break;  

    }
}




esp_err_t websocket_client_start(const char *session_id, const char *email, const char *password , const char *node_name)
{
    if (client) {
        ESP_LOGW(TAG, "WebSocket client already started");
        return ESP_OK;
    }   

    strncpy(ws_email, email, sizeof(ws_email) - 1);
    strncpy(ws_password, password, sizeof(ws_password) - 1);
    strncpy(ws_node_name, node_name, sizeof(ws_node_name) - 1);
    strncpy(ws_session_id, session_id, sizeof(ws_session_id)-1);

    char uri[512];
   // snprintf(uri, sizeof(uri), "wss://dev-corid.cor-medical.ua/dev-modbus/devices?session_id=%s", session_id);
    snprintf(uri, sizeof(uri), "%s%s", sys.ws_server, session_id);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        .cert_pem = (const char *)ca_cert_pem_start,
        .reconnect_timeout_ms = 90000,
        .network_timeout_ms = 10000,
        .skip_cert_common_name_check = true,
    };

    client = esp_websocket_client_init(&websocket_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(
        esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL)
    );

    esp_err_t err = esp_websocket_client_start(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "🚀 WebSocket client started: %s", uri);

    return ESP_OK;
}



esp_err_t websocket_client_send(const char *message)
{
    if (client && esp_websocket_client_is_connected(client)) {
        esp_websocket_client_send_text(client, message, strlen(message), portMAX_DELAY);
        ESP_LOGI(TAG, "Sent message: %s", message);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "WebSocket not connected, can't send message");
        return ESP_FAIL;
    }
}

void websocket_client_stop(void)
{
    if (client) {
        gpio_set_net_led(false);
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
        ESP_LOGI(TAG, "WebSocket client stopped");
    }
}


void initialize_sntp(void)
{
    ESP_LOGI("SNTP", "Init SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Ждём синхронизации, но максимум 30 секунд
    time_t now;
    struct tm timeinfo = { 0 };
    const int max_wait_sec = 30;

    for (int i = 0; i < max_wait_sec; i++) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2020 - 1900)) {
            ESP_LOGI("TIME", "✅ SNTP synced: %s", asctime(&timeinfo));
            return;
        }
        ESP_LOGI("SNTP", "Waiting for time... (%d/%d)", i + 1, max_wait_sec);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGW("TIME", "❌ SNTP sync timeout, continuing with default time");
}


int hex_to_bytes(const char *in, uint8_t *out, int max_len)
{
    int len = strlen(in);
    if (len % 2 != 0) return -1;

    int out_len = len / 2;
    if (out_len > max_len) return -1;

    for (int i = 0; i < out_len; i++) {
        sscanf(in + 2*i, "%2hhx", &out[i]);
    }
    return out_len;
}



 void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size)
{
    int pos = 0;
    for (int i = 0; i < len; i++) {
        pos += snprintf(out + pos, out_size - pos, "%02X", data[i]);
    }
}



bool websocket_send_text(const char *msg)
{
    if (!ws_connected || !client) {
        ESP_LOGW(TAG, "⚠️ WebSocket not connected, drop message");
        return false;
    }

    esp_err_t ret = websocket_client_send( msg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Send failed: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}


void websocket_restart(const char *email, const char *password, const char *node_name)
{
    ESP_LOGW(TAG, "🔄 Restarting WebSocket client with new credentials...");

     ws_reconnect_in_progress = true;
    // Обновляем глобальные переменные
    strncpy(ws_email, email, sizeof(ws_email)-1);
    strncpy(ws_password, password, sizeof(ws_password)-1);
    strncpy(ws_node_name, node_name, sizeof(ws_node_name)-1);

    // Останавливаем клиент, если запущен
    if (client) {
        esp_websocket_client_stop(client);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_destroy(client);
        client = NULL;
    }

    ws_connected = false;
    

    // Стартуем заново
    esp_err_t ok = websocket_client_start(user_cfg.serial, ws_email, ws_password, user_cfg.node_name);

    if (ok == ESP_OK) {
        ESP_LOGI(TAG, "✅ WebSocket restarted successfully");
    } else {
        ESP_LOGE(TAG, "❌ WebSocket restart failed: %s", esp_err_to_name(ok));
    }
   //  ws_reconnect_in_progress = false;
}



void get_time_iso(char *buf, size_t len)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &timeinfo);
}