

#include "network_state.h"
#include "ws_server.h"  
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "NET_STATE";

static net_state_t g_wifi_state = NET_STATE_WIFI_DOWN;
static net_state_t g_eth_state  = NET_STATE_ETHERNET_DOWN;
static SemaphoreHandle_t net_state_mutex = NULL;



const char* net_state_to_str(net_state_t state)
{
    switch (state) {
        case NET_STATE_WIFI_UP:               return "up";
        case NET_STATE_WIFI_CONNECTING:       return "connecting";
        case NET_STATE_WIFI_DOWN:             return "down";
        case NET_STATE_ETHERNET_UP:            return "up";
        case NET_STATE_ETHERNET_CONNECTING:    return "connecting";
        case NET_STATE_ETHERNET_DOWN:          return "down";
        default: return "unknown";
    }
}

void network_notify_ws(void)
{
    net_state_t wifi = network_get_wifi_state();
    net_state_t eth  = network_get_ethernet_state();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "network");

    cJSON *network = cJSON_AddObjectToObject(root, "network");

    // --- Ethernet ---
    cJSON *eth_obj = cJSON_AddObjectToObject(network, "ethernet");
    cJSON_AddStringToObject(eth_obj, "state", net_state_to_str(eth));

    // --- Wi-Fi STA ---
    cJSON *wifi_sta_obj = cJSON_AddObjectToObject(network, "wifi_sta");
    cJSON_AddStringToObject(wifi_sta_obj, "state", net_state_to_str(wifi));

    // --- Wi-Fi AP (позже)
    // cJSON *wifi_ap_obj = cJSON_AddObjectToObject(network, "wifi_ap");
    // cJSON_AddStringToObject(wifi_ap_obj, "state", net_state_to_str(ap));

    char *json = cJSON_PrintUnformatted(root);
    ws_broadcast(json);

    free(json);
    cJSON_Delete(root);
}




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

    if (g_wifi_state != state) {
        g_wifi_state = state;
        xSemaphoreGive(net_state_mutex);
        network_notify_ws();   // 🔥 ТОЛЬКО ПРИ ИЗМЕНЕНИИ
        return;
    }

    xSemaphoreGive(net_state_mutex);
}



void network_set_ethernet_state(net_state_t state)
{
    xSemaphoreTake(net_state_mutex, portMAX_DELAY);

    if (g_eth_state != state) {
        g_eth_state = state;
        xSemaphoreGive(net_state_mutex);
        network_notify_ws();
        return;
    }

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






