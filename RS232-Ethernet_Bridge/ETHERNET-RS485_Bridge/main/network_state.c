

#include "network_state.h"
#include "ws_server.h"  
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/netif.h"

#include "websocket_client.h"


#define RSSI_SEND_INTERVAL_MS 5000     // не чаще чем раз в 5 сек
#define RSSI_DELTA_THRESHOLD 3         // минимум изменение на 3 dBm


static const char *TAG = "NET_STATE";

static net_state_t g_wifi_state = NET_STATE_WIFI_DOWN;
static net_state_t g_eth_state  = NET_STATE_ETHERNET_DOWN;
static SemaphoreHandle_t net_state_mutex = NULL;

static int g_wifi_rssi = -100;
static int g_wifi_rssi_last_sent = -100;
static int64_t g_last_rssi_send_time = 0;

extern int g_eth_speed_mbps;
extern float g_eth_rx_kbps;
extern float g_eth_tx_kbps;



void network_update_wifi_rssi(int rssi)
{
    int64_t now = esp_timer_get_time() / 1000; // ms

    // Фильтр по времени
    if ((now - g_last_rssi_send_time) < RSSI_SEND_INTERVAL_MS) {
        return;
    }

    // Фильтр по изменению
    if (abs(rssi - g_wifi_rssi_last_sent) < RSSI_DELTA_THRESHOLD) {
        return;
    }

    g_wifi_rssi = rssi;
    g_wifi_rssi_last_sent = rssi;
    g_last_rssi_send_time = now;

    network_notify_ws(); // отправляем обновление
}


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
    cJSON_AddNumberToObject(eth_obj, "speed_mbps", g_eth_speed_mbps);
    cJSON_AddNumberToObject(eth_obj, "rx_kbps", g_eth_rx_kbps);
    cJSON_AddNumberToObject(eth_obj, "tx_kbps", g_eth_tx_kbps);
    // --- Wi-Fi STA ---
    cJSON *wifi_sta_obj = cJSON_AddObjectToObject(network, "wifi_sta");
    cJSON_AddStringToObject(wifi_sta_obj, "state", net_state_to_str(wifi));
    cJSON_AddNumberToObject(wifi_sta_obj, "rssi", g_wifi_rssi);
    // --- Wi-Fi AP (позже)
    // cJSON *wifi_ap_obj = cJSON_AddObjectToObject(network, "wifi_ap");
    // cJSON_AddStringToObject(wifi_ap_obj, "state", net_state_to_str(ap));

    char *json = cJSON_PrintUnformatted(root);
    ws_broadcast(json);
    websocket_send_text(json);

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



bool network_is_ready(void)
{
    net_state_t wifi = network_get_wifi_state();
    net_state_t eth  = network_get_ethernet_state();

    // Считаем сеть готовой, если хотя бы один интерфейс UP
    if (wifi == NET_STATE_WIFI_UP || eth == NET_STATE_ETHERNET_UP) {
        return true;
    }

    return false;
}


