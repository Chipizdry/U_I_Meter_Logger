


#include "pairing_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "wifi_manager.h"
#include "ethernet_manager.h"
#include "web_server.h"
#include "ws_server.h"
#include "dns_server.h"
#include "websocket_client.h"     // websocket_stop()

static const char *TAG = "PAIRING_MANAGER";
static SemaphoreHandle_t pairing_mutex;
static bool pairing_initialized = false;
static volatile bool pairing_active = false;

bool pairing_is_active(void) { return pairing_active; }

static esp_err_t pairing_manager_init(void)
{
    if (pairing_initialized) return ESP_OK;
    pairing_mutex = xSemaphoreCreateMutex();
    if (!pairing_mutex) return ESP_ERR_NO_MEM;
    pairing_initialized = true;
    return ESP_OK;
}

static void stop_network_services(void)
{
    ESP_LOGI(TAG, "Stopping network services...");

    /* 1. Останавливаем входящие сервисы */
    ws_cleanup_all_clients();
    web_server_stop();
    dns_stop();

    /* 2. Рвём сетевой транспорт СНАЧАЛА — WS-клиент потеряет связь
     *    и уйдёт в реконнект, но не будет активно работать. */
    wifi_manager_stop();           /* с патчем из п.6 (pause) */
    ethernet_deinit();

    /* 3. Даём WS-клиенту 2 с на обнаружение разрыва и на cleanup
     *    (его задача сама вызовет destroy_and_free_resources и завершится) */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 4. НЕ вызываем websocket_stop() — здесь он делает больше вреда,
     *    чем пользы. esp_restart() через несколько секунд всё вычистит. */

    ESP_LOGI(TAG, "All network services stopped");
}


static void pairing_task(void *arg)
{
    ESP_LOGW(TAG, "================ PAIRING MODE ================");

    stop_network_services();

    if (ble_start_pairing_mode() != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed");
        pairing_active = false;
        esp_restart();
        return;
    }

    /* Ожидание завершения обмена (с таймаутом) */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PAIRING_TIMEOUT_MS);
    while (!ble_pairing_is_done() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (ble_pairing_is_done() && ble_pairing_was_successful()) {
        ESP_LOGI(TAG, "✅ Pairing OK — token saved to NVS");
        /* Даём телефону время прочитать TX-ответ */
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else if (ble_pairing_is_done()) {
        ESP_LOGW(TAG, "❌ Pairing failed (validation error)");
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGW(TAG, "⏱ Pairing timeout");
    }

    ble_stop_pairing_mode();
    pairing_active = false;

    ESP_LOGW(TAG, "Rebooting to normal mode...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t pairing_manager_request(void)
{
    esp_err_t err = pairing_manager_init();
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(pairing_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (pairing_active) {
        ESP_LOGW(TAG, "Pairing already active");
        xSemaphoreGive(pairing_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    pairing_active = true;
    xSemaphoreGive(pairing_mutex);

    ESP_LOGI(TAG, "Pairing requested — starting task");
    if (xTaskCreate(pairing_task, "pairing_task", 8192, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pairing_task");
        pairing_active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}



