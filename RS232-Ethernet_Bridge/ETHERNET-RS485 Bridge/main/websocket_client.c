





#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"

#include "nvs_settings.h"
#include "rs485_master.h"
#include "ws_server.h"
#include "gpio_manager.h"

 TickType_t last_ws_event_tick = 0; // последняя активность WebSocket

static uint32_t last_modbus_tick = 0;
static uint32_t last_pi30_tick   = 0;

static const TickType_t WS_TIMEOUT_TICKS = pdMS_TO_TICKS(15000); // 10 секунд
// переменные для авторизации
 char ws_email[64];
 char ws_password[64];
 char ws_node_name[64];

static bool ws_reconnect_in_progress = false;
extern bool test_account_active;// из ws_server.c 
extern void ws_broadcast(const char *text);
extern httpd_handle_t server;

extern esp_err_t rs485_master_send(const uint8_t *data, size_t len);
// Встроенный сертификат (объявляется линковщиком)
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

static const char *TAG = "websocket_client";
 esp_websocket_client_handle_t client = NULL;

bool ws_connected = false;
static bool ws_reconnect_enabled = true;

char cloud_status_msg[32] = "Idle";   // статус по умолчанию
static char ws_rx_buf[512];
static int ws_rx_len = 0;


static bool handle_hex_data(const char *json);
static bool handle_pi30_data(const char *json);


static void websocket_start(void);
 void websocket_reconnect_task(void *pvParameters);

 static inline uint32_t ticks_to_ms(uint32_t ticks)
{
    return ticks * portTICK_PERIOD_MS;
}


void websocket_disable_reconnect(void)
{
    ws_reconnect_enabled = false;
    ws_reconnect_in_progress = false;
}

void websocket_enable_reconnect(void)
{
    ws_reconnect_enabled = true;
}


 void websocket_reconnect_task(void *pvParameters)
{
    const TickType_t check_interval = pdMS_TO_TICKS(1000); // проверяем каждую секунду

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
                ESP_LOGW(TAG, "⚠️ No WS activity for 10 seconds, forcing reconnect");
                need_reconnect = true;
            }
        }

        if (need_reconnect) 
{
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

    const char *email_to_use = test_account_active ? ws_email : user.account_login;
    const char *pass_to_use  = test_account_active ? ws_password : user.account_password;
    const char *node_name_use  = test_account_active ? ws_node_name : user.node_name;


    esp_err_t ok = websocket_client_start(user.serial, email_to_use, pass_to_use, node_name_use);
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

static int hex_to_bytes(const char *in, uint8_t *out, int max_len);
void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size);



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
             gpio_set_net_led(true);
            ws_broadcast("{\"cloud_status\":\"Connecting...\"}");
             last_ws_event_tick = xTaskGetTickCount(); // фиксируем активность
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
            {
                last_ws_event_tick = xTaskGetTickCount();

                if (!data->data_ptr || data->data_len <= 0) return;

                if (ws_rx_len + data->data_len < sizeof(ws_rx_buf) - 1) {
                    memcpy(ws_rx_buf + ws_rx_len, data->data_ptr, data->data_len);
                    ws_rx_len += data->data_len;
                    ws_rx_buf[ws_rx_len] = 0;
                }

                if (!data->fin) return;

                ESP_LOGI(TAG, "As string: %s", ws_rx_buf);
            
                // === Ретрансляция статусов от облака ===
                if (strstr(ws_rx_buf, "\"cloud_status\"")) 
               
                {
                ESP_LOGI(TAG, "📡 Broadcasting cloud status to local WS clients: %s", ws_rx_buf);
                ws_broadcast(ws_rx_buf);

                      // ---- NEW: сохранить статус для get_settings ----
                    char *ptr = strstr(ws_rx_buf, "\"cloud_status\"");
                    if (ptr) {
                        char *start = strchr(ptr, ':');
                        if (start && (start = strchr(start, '"'))) {
                            start++;
                            char *end = strchr(start, '"');
                            if (end && end > start) {
                                size_t len = end - start;
                                if (len < sizeof(cloud_status_msg)) {
                                    memcpy(cloud_status_msg, start, len);
                                    cloud_status_msg[len] = 0;
                                    ESP_LOGI(TAG, "💾 Saved cloud_status_msg = %s", cloud_status_msg);
                                    if (strcmp(cloud_status_msg, "authenticated") == 0 ||
                                            strcmp(cloud_status_msg, "connected") == 0) {
                                            ws_connected = true;
                                        } else {
                                            ws_connected = false;
                                        }
                                   
                                }
                            }
                        }
                    }

                }

                handle_hex_data(ws_rx_buf);
                handle_pi30_data(ws_rx_buf);

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

    char uri[256];
    snprintf(uri, sizeof(uri), "wss://dev-corid.cor-medical.ua/dev-modbus/devices?session_id=%s", session_id);

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
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = { 0 };

    while (timeinfo.tm_year < (2020 - 1900)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    ESP_LOGI("TIME", "✅ SNTP time synced: %s", asctime(&timeinfo));
}


static int hex_to_bytes(const char *in, uint8_t *out, int max_len)
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
    esp_err_t ok = websocket_client_start(user.serial, ws_email, ws_password, user.node_name);

    if (ok == ESP_OK) {
        ESP_LOGI(TAG, "✅ WebSocket restarted successfully");
    } else {
        ESP_LOGE(TAG, "❌ WebSocket restart failed: %s", esp_err_to_name(ok));
    }
     ws_reconnect_in_progress = false;
}




static bool handle_hex_data(const char *json)
{
    char *hex_ptr = strstr(json, "\"hex_data\"");
    if (!hex_ptr) {
        return false;
    }

    char *start = strchr(hex_ptr, ':');
    if (!start || !(start = strchr(start, '"'))) {
        ESP_LOGE(TAG, "hex_data: format error");
        return true;
    }
    start++;

    char *end = strchr(start, '"');
    if (!end || end <= start) {
        ESP_LOGE(TAG, "hex_data: empty value");
        return true;
    }

    char hex_str[128] = {0};
    int len = end - start;
    if (len >= sizeof(hex_str)) {
        ESP_LOGE(TAG, "hex_data too long");
        return true;
    }

    memcpy(hex_str, start, len);
    ESP_LOGI(TAG, "✅ HEX extracted: %s", hex_str);

    // убираем пробелы
    char clean_hex[128] = {0};
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (hex_str[i] != ' ') {
            clean_hex[j++] = hex_str[i];
        }
    }

    // диагностика времени
    uint32_t now = xTaskGetTickCount();
    uint32_t delta_ms = last_modbus_tick ? ticks_to_ms(now - last_modbus_tick) : 0;
    last_modbus_tick = now;

    if (diagnostics_active && delta_ms > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "{\"diag\":\"Modbus update : %lu\"}", delta_ms);
        ws_broadcast(msg);
    }

    uint8_t bytes[64];
    int byte_len = hex_to_bytes(clean_hex, bytes, sizeof(bytes));
    if (byte_len <= 0) {
        ESP_LOGE(TAG, "HEX parse error");
        return true;
    }

    ESP_LOGI(TAG, "📤 RS485 send %d bytes", byte_len);
    ESP_LOG_BUFFER_HEX(TAG, bytes, byte_len);
    rs485_master_send(bytes, byte_len);

    return true;
}



static bool handle_pi30_data(const char *json)
{
    char *pi30_ptr = strstr(json, "\"pi30\"");
    if (!pi30_ptr) {
        return false;
    }

    char *start = strchr(pi30_ptr, ':');
    if (!start || !(start = strchr(start, '"'))) {
        ESP_LOGE(TAG, "pi30: format error");
        return true;
    }
    start++;

    char *end = strchr(start, '"');
    if (!end || end <= start) {
        ESP_LOGE(TAG, "pi30: empty value");
        return true;
    }

    char hex_str[128] = {0};
    int len = end - start;
    if (len >= sizeof(hex_str)) {
        ESP_LOGE(TAG, "pi30 too long");
        return true;
    }

    memcpy(hex_str, start, len);
    //ESP_LOGI(TAG, "🔶 PI30 HEX: %s", hex_str);

    char clean_hex[128] = {0};
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (hex_str[i] != ' ' && hex_str[i] != '\n') {
            clean_hex[j++] = hex_str[i];
        }
    }

    uint32_t now = xTaskGetTickCount();
    uint32_t delta_ms = last_pi30_tick ? ticks_to_ms(now - last_pi30_tick) : 0;
    last_pi30_tick = now;

    if (diagnostics_active && delta_ms > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "{\"diag\":\"PI30 update : %lu\"}", delta_ms);
        ws_broadcast(msg);
    }

    uint8_t bytes[64];
    int byte_len = hex_to_bytes(clean_hex, bytes, sizeof(bytes));
    if (byte_len <= 0) {
        ESP_LOGE(TAG, "PI30 HEX parse error");
        return true;
    }

    // ASCII лог
    char ascii[128];
    int ai = 0;
    for (int i = 0; i < byte_len; i++) {
        char c = bytes[i];
        ascii[ai++] = (c >= 32 && c < 127) ? c : '.';
    }
    ascii[ai] = 0;

    ESP_LOGI(TAG, "🔤 PI30 ASCII: %s", ascii);
  //  ESP_LOGI(TAG, "📤 UART send %d bytes", byte_len);
  //  ESP_LOG_BUFFER_HEX(TAG, bytes, byte_len);

    rs485_master_send(bytes, byte_len);
    return true;
}
