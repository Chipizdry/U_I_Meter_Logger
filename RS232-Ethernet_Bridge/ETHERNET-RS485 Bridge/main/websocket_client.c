


#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_sntp.h"

// Встроенный сертификат (объявляется линковщиком)
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

static const char *TAG = "websocket_client";
static esp_websocket_client_handle_t client = NULL;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ Connected to WebSocket server");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️  Disconnected from WebSocket server");
            break;

        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(TAG, "📩 Received message (%d bytes): %.*s", data->data_len, data->data_len, (char *)data->data_ptr);
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket error occurred");
            break;

        default:
            break;
    }
}



esp_err_t websocket_client_start(const char *uri)
{
    if (client) {
        ESP_LOGW(TAG, "WebSocket client already started");
        return ESP_OK;
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        .cert_pem = (const char *)ca_cert_pem_start,  // ✅ используем наш CA сертификат
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .skip_cert_common_name_check = true,           // если CN не совпадает с хостом
    };

    client = esp_websocket_client_init(&websocket_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "🚀 WebSocket client started: %s", uri);
    xTaskCreate(websocket_send_task, "websocket_send_task", 4096, NULL, 5, NULL);
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
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // ждем, пока время установится
    time_t now = 0;
    struct tm timeinfo = { 0 };
    while (timeinfo.tm_year < (2020 - 1900)) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    ESP_LOGI("TIME", "✅ System time set to: %s", asctime(&timeinfo));
}