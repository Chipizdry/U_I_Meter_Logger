

#include "network_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "NET_STATE";

static net_state_t g_wifi_state = NET_STATE_WIFI_DOWN;
static net_state_t g_eth_state  = NET_STATE_ETHERNET_DOWN;
static SemaphoreHandle_t net_state_mutex = NULL;

void network_state_init(void)
{
    net_state_mutex = xSemaphoreCreateMutex();
     g_wifi_state = NET_STATE_WIFI_DOWN;
     g_eth_state  = NET_STATE_ETHERNET_DOWN;
     
    if (!net_state_mutex) {
        ESP_LOGE(TAG, "Failed to create net_state_mutex");
        abort(); // это фатальная ошибка
    }
    ESP_LOGI(TAG, "Network state initialized");
}

void network_set_wifi_state(net_state_t state)
{
    xSemaphoreTake(net_state_mutex, portMAX_DELAY);
    g_wifi_state = state;
    xSemaphoreGive(net_state_mutex);
}

void network_set_ethernet_state(net_state_t state)
{
    xSemaphoreTake(net_state_mutex, portMAX_DELAY);
    g_eth_state = state;
    xSemaphoreGive(net_state_mutex);
}

net_state_t network_get_wifi_state(void)
{
    net_state_t state;
    xSemaphoreTake(net_state_mutex, portMAX_DELAY);
    state = g_wifi_state;
    xSemaphoreGive(net_state_mutex);
    return state;
}

net_state_t network_get_ethernet_state(void)
{
    net_state_t state;
    xSemaphoreTake(net_state_mutex, portMAX_DELAY);
    state = g_eth_state;
    xSemaphoreGive(net_state_mutex);
    return state;
}






