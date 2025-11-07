


#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"

#include "nvs_settings.h"
#include "rs485_master.h"


extern esp_err_t rs485_master_send(const uint8_t *data, size_t len);
// Встроенный сертификат (объявляется линковщиком)
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

static const char *TAG = "websocket_client";
static esp_websocket_client_handle_t client = NULL;

static bool ws_connected = false;
static int reconnect_delay_sec = 3;

static void websocket_start(void);


static void websocket_reconnect(void)
{
    ws_connected = false;

    if (client) {
        esp_websocket_client_close(client, 1000 / portTICK_PERIOD_MS);
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
    }

    ESP_LOGW(TAG, "🔁 Reconnecting to WebSocket in %d sec...", reconnect_delay_sec);
    vTaskDelay(pdMS_TO_TICKS(reconnect_delay_sec * 1000));

    reconnect_delay_sec *= 2;
    if (reconnect_delay_sec > 30) reconnect_delay_sec = 30;

    websocket_client_start(user.serial, user.account_login, user.account_password);
}




static int hex_to_bytes(const char *in, uint8_t *out, int max_len);
static void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size);

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

            // отправляем авторизационное сообщение
            if (strlen(ws_email) > 0 && strlen(ws_password) > 0) {
                char auth_msg[256];
                snprintf(auth_msg, sizeof(auth_msg),
                        "{\"email\": \"%s\", \"password\": \"%s\"}",
                        ws_email, ws_password);

                esp_websocket_client_send_text(client, auth_msg, strlen(auth_msg), portMAX_DELAY);
                ESP_LOGI(TAG, "📤 Sent auth message: %s", auth_msg);
            }
        break;


        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️ WebSocket disconnected!");
            websocket_reconnect();
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket error!");
            websocket_reconnect();
            break;

        case WEBSOCKET_EVENT_DATA:
            {
                ESP_LOGI(TAG, "----- WebSocket Packet -----");
                ESP_LOGI(TAG, "Opcode: %d", data->op_code);
                ESP_LOGI(TAG, "Payload len: %d", data->data_len);
                ESP_LOGI(TAG, "FIN: %d", data->fin);
            
                // Если текст
                if (data->op_code == 1 || data->op_code == 0) {
                 
                    ESP_LOGI(TAG, "As string: %.*s", data->data_len, (char *)data->data_ptr);
                }
                // Если бинарные данные
                else if (data->op_code == 2) {
                    ESP_LOGI(TAG, "Binary message:");
                    ESP_LOG_BUFFER_HEXDUMP(TAG, data->data_ptr, data->data_len, ESP_LOG_INFO);
                }
            
                // Если FIN==1 — значит это последний фрагмент
                if (data->fin) {
                    ESP_LOGI(TAG, "----- End of packet -----");
                }
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

    xTaskCreate(websocket_send_task, "ws_send_task", 4096, NULL, 5, NULL);

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



static void bytes_to_hex(const uint8_t *data, int len, char *out, int out_size)
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

    if (esp_websocket_client_send_text(client, msg, strlen(msg), 1000) != ESP_OK) {
        ESP_LOGE(TAG, "❌ Send failed, reconnecting...");
        websocket_reconnect();
        return false;
    }
    return true;
}