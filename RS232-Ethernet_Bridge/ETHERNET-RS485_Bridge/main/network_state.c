

#include "network_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "NET_STATE";

static net_state_t g_net_state = NET_STATE_DOWN;
static SemaphoreHandle_t net_state_mutex = NULL;

void network_state_init(void)
{
    net_state_mutex = xSemaphoreCreateMutex();
     g_net_state = NET_STATE_DOWN;
    if (!net_state_mutex) {
        ESP_LOGE(TAG, "Failed to create net_state_mutex");
        abort(); // это фатальная ошибка
    }
    ESP_LOGI(TAG, "Network state initialized");
}

void network_set_state(net_state_t state)
{
     ESP_LOGI(TAG, "Network state changed: %d", state);
    if (!net_state_mutex) return;

    xSemaphoreTake(net_state_mutex, portMAX_DELAY);
    g_net_state = state;
    xSemaphoreGive(net_state_mutex);
}

net_state_t network_get_state(void)
{
    if (!net_state_mutex) return NET_STATE_DOWN;
     if (net_state_mutex == NULL) {
        return NET_STATE_DOWN;   // безопасное значение
    }

    net_state_t state;
    xSemaphoreTake(net_state_mutex, portMAX_DELAY);
    state = g_net_state;
    xSemaphoreGive(net_state_mutex);
    return state;
}





