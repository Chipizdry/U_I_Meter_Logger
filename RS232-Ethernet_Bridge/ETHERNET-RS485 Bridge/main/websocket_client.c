


#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"

#include "nvs_settings.h"
#include "rs485_master.h"

static TickType_t last_ws_event_tick = 0; // последняя активность WebSocket
static const TickType_t WS_TIMEOUT_TICKS = pdMS_TO_TICKS(7000); // 7 секунд

extern esp_err_t rs485_master_send(const uint8_t *data, size_t len);
// Встроенный сертификат (объявляется линковщиком)
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

static const char *TAG = "websocket_client";
static esp_websocket_client_handle_t client = NULL;

bool ws_connected = false;
char ws_status_msg[128] = "Idle";
static char ws_rx_buf[512];
static int ws_rx_len = 0;

static void websocket_start(void);
 void websocket_reconnect_task(void *pvParameters);


 void websocket_reconnect_task(void *pvParameters)
{
    const TickType_t check_interval = pdMS_TO_TICKS(1000); // проверяем каждую секунду

    for (;;)
    {
        bool need_reconnect = false;

        if (!ws_connected) {
            need_reconnect = true;
        } else {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ws_event_tick) > WS_TIMEOUT_TICKS) {
                ESP_LOGW(TAG, "⚠️ No WS activity for 7 seconds, forcing reconnect");
                need_reconnect = true;
            }
        }

        if (need_reconnect) 
        {
            if (client) 
            {
                esp_websocket_client_stop(client);
                vTaskDelay(pdMS_TO_TICKS(100));  // ждем корректного завершения
                esp_websocket_client_destroy(client);
                client = NULL;
                ws_connected = false;
            }

            if (websocket_client_start(user.serial, user.account_login, user.account_password) == ESP_OK) 
            {
                ESP_LOGI(TAG, "✅ WebSocket reconnect initiated");
            }

            last_ws_event_tick = xTaskGetTickCount(); // сброс таймера после реконнекта
        }

        vTaskDelay(check_interval);
    }
}

static int hex_to_bytes(const char *in, uint8_t *out, int max_len);
void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size);

// переменные для авторизации
static char ws_email[64];
static char ws_password[64];



static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
        case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "✅ Connected to WebSocket server");
        last_ws_event_tick = xTaskGetTickCount(); // сброс таймера при подключении
            // отправляем авторизационное сообщение
            if (strlen(ws_email) > 0 && strlen(ws_password) > 0) {
                char auth_msg[256];
                snprintf(auth_msg, sizeof(auth_msg),
                        "{\"email\": \"%s\", \"password\": \"%s\"}",
                        ws_email, ws_password);

                esp_websocket_client_send_text(client, auth_msg, strlen(auth_msg), portMAX_DELAY);
                ESP_LOGI(TAG, "📤 Sent auth message: %s", auth_msg);
            }
            ws_connected = true;
            strncpy(ws_status_msg, "Connected", sizeof(ws_status_msg));
        break;


        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️ WebSocket disconnected!");
            ws_connected = false;
            strncpy(ws_status_msg, "Disconnected", sizeof(ws_status_msg));
           
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket error!");
            ws_connected = false;
            strncpy(ws_status_msg, "Error", sizeof(ws_status_msg));
            break;

            case WEBSOCKET_EVENT_DATA:
            {

                last_ws_event_tick = xTaskGetTickCount(); // фиксируем активность
                if (data->data_len <= 0 || data->data_ptr == NULL) {
                    return;
                }
            
                // Копим фрагменты в буфер
                if (ws_rx_len + data->data_len < sizeof(ws_rx_buf) - 1) {
                    memcpy(ws_rx_buf + ws_rx_len, data->data_ptr, data->data_len);
                    ws_rx_len += data->data_len;
                    ws_rx_buf[ws_rx_len] = 0;
                }
            
                // Ждём финальный фрагмент
                if (!data->fin) {
                    ESP_LOGD(TAG, "WS fragment received, waiting more...");
                    return;
                }
            
                ESP_LOGI(TAG, "----- WebSocket Packet (assembled) -----");
                ESP_LOGI(TAG, "As string: %s", ws_rx_buf);
            
                // === Парсинг hex_data ===
                char *hex_ptr = strstr(ws_rx_buf, "\"hex_data\"");
                if (hex_ptr) {
                    char *start = strchr(hex_ptr, ':');
                    if (start && (start = strchr(start, '"'))) {
                        start++;
                        char *end = strchr(start, '"');
                        if (end && end > start) {
                            char hex_str[128] = {0};
                            int hex_len = end - start;
                            if (hex_len < sizeof(hex_str)) {
                                memcpy(hex_str, start, hex_len);
                                ESP_LOGI(TAG, "✅ HEX string extracted: %s", hex_str);
            
                                char clean_hex[128] = {0};
                                int j = 0;
                                for (int i = 0; i < strlen(hex_str); i++) {
                                    if (hex_str[i] != ' ') clean_hex[j++] = hex_str[i];
                                }
            
                              //  ESP_LOGI(TAG, "🔧 Clean HEX: %s", clean_hex);
            
                                uint8_t bytes[64];
                                int byte_len = hex_to_bytes(clean_hex, bytes, sizeof(bytes));
            
                                if (byte_len > 0) {
                                    ESP_LOGI(TAG, "📤 Sending %d bytes to RS485:", byte_len);
                                    ESP_LOG_BUFFER_HEX(TAG, bytes, byte_len);
                                    rs485_master_send(bytes, byte_len);
                                } else {
                                    ESP_LOGE(TAG, "❌ HEX parse error");
                                }
                            }
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "⚠️ No hex_data found in message");
                }
            
                ESP_LOGI(TAG, "----- End of packet -----");
            
                // очищаем буфер для следующего сообщения
                ws_rx_len = 0;
                ws_rx_buf[0] = 0;
            }
            break;
    }
}




esp_err_t websocket_client_start(const char *session_id, const char *email, const char *password)
{
    if (client) {
        ESP_LOGW(TAG, "WebSocket client already started");
        return ESP_OK;
    }

    strncpy(ws_email, email, sizeof(ws_email) - 1);
    strncpy(ws_password, password, sizeof(ws_password) - 1);

    char uri[256];
    snprintf(uri, sizeof(uri),
             "wss://dev-corid.cor-medical.ua/dev-modbus/devices?session_id=%s",
             session_id);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        .cert_pem = (const char *)ca_cert_pem_start,
        .reconnect_timeout_ms = 5000,
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

  //  xTaskCreate(websocket_send_task, "ws_send_task", 4096, NULL, 5, NULL);

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
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
        ESP_LOGI(TAG, "WebSocket client stopped");
    }
}

/*
static void websocket_send_task(void *pvParameters)
{
    int counter = 0;
    while (1)
    {
        if (esp_websocket_client_is_connected(client))
        {
            char message[128];
            snprintf(message, sizeof(message), "{\"msg_id\": %d, \"text\": \"Hello from ESP32 #%d\"}", counter, counter);
            esp_websocket_client_send_text(client, message, strlen(message), portMAX_DELAY);
            ESP_LOGI(TAG, "📤 Sent test message: %s", message);
            counter++;
        }
        else
        {
            ESP_LOGW(TAG, "⚠️ WebSocket not connected, skipping send");
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); 
    }
}
*/



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


