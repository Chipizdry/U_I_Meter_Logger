

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <string.h>

#define MAX_APs 20
static const char *TAG = "wifi_manager";
static wifi_ap_record_t ap_list[MAX_APs];
static uint16_t ap_count = 0;



// --- Событийный обработчик ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
    int32_t event_id, void *event_data)
{
if (event_base == WIFI_EVENT) {
switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Connected to AP");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
        break;
    case WIFI_EVENT_SCAN_DONE:
        {
        ESP_LOGI(TAG, "Scan done");
        uint16_t count = MAX_APs;
        esp_wifi_scan_get_ap_records(&count, ap_list);
        ap_count = count;
        ESP_LOGI(TAG, "Found %d APs:", ap_count);
        for (int i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "%d: SSID:%s, RSSI:%d, Auth:%d, Channel:%d",
        i+1, ap_list[i].ssid, ap_list[i].rssi,
        ap_list[i].authmode, ap_list[i].primary);
        }
        break;
        }
    default:
        break;
        }
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address");
        }
}



esp_err_t wifi_manager_init(const wifi_settings_t *cfg)
{
    //ESP_ERROR_CHECK(esp_netif_init());
   // ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *netif = NULL;

    if (cfg->mode == WIFI_MODE_STA || cfg->mode == WIFI_MODE_APSTA) {
        netif = esp_netif_create_default_wifi_sta();
    }
    if (cfg->mode == WIFI_MODE_AP || cfg->mode == WIFI_MODE_APSTA) {
        esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, cfg->ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, cfg->password, sizeof(sta_cfg.sta.password));

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, cfg->ap_password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
    ap_cfg.ap.channel = cfg->ap_channel;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(cfg->ap_password) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(cfg->mode));
    if (cfg->mode == WIFI_MODE_STA) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    } else if (cfg->mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    } else if (cfg->mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi initialized in mode %d", cfg->mode);
    return ESP_OK;
}


// --- Неблокирующее сканирование ---
esp_err_t wifi_scan_networks(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    // false = неблокирующий вызов, результаты придут через событие WIFI_EVENT_SCAN_DONE
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, false));
    ESP_LOGI(TAG, "Started Wi-Fi scan (non-blocking)");
    return ESP_OK;
}

// --- Получение списка AP ---
const wifi_ap_record_t* wifi_get_ap_list(uint16_t *count)
{
    if (count) *count = ap_count;
    return ap_list;
}




// --- Подключение к выбранной сети ---
esp_err_t wifi_connect_to(const char* ssid, const char* password)
{
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return ESP_OK;
}

void wifi_manager_stop(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "Wi-Fi stopped");
}








