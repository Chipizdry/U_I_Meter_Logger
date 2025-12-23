

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "ws_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define MAX_APs 20
static const char *TAG = "wifi_manager";
static wifi_ap_record_t ap_list[MAX_APs];
static uint16_t ap_count = 0;

static EventGroupHandle_t wifi_evt_group;
#define WIFI_EVT_APPLY   BIT0

extern void ws_broadcast(const char *text);
extern void ws_send(int client_fd, const char *text);

static SemaphoreHandle_t ap_list_mutex = NULL;

// --- Forward ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void check_and_stop_wifi(void);

// --- Проверка и остановка Wi-Fi ---
static void check_and_stop_wifi(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi stopped successfully");
    }
    esp_wifi_deinit();
    ESP_LOGI(TAG, "Wi-Fi deinitialized");
}



static const char* wifi_reason_to_str(wifi_err_reason_t reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "Unspecified";
        case WIFI_REASON_AUTH_EXPIRE: return "Auth expired";
        case WIFI_REASON_NO_AP_FOUND: return "AP not found";
        case WIFI_REASON_AUTH_FAIL: return "Auth failed";
        case WIFI_REASON_ASSOC_FAIL: return "Association failed";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Handshake timeout";
        default: return "Other";
    }
}



// --- Инициализация STA ---
void init_wifi_sta(const wifi_settings_t *cfg)
{
    ESP_LOGI(TAG, "Initializing STA mode...");

    check_and_stop_wifi();

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    assert(netif);

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    // Регистрация обработчиков для STA
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, cfg->sta_ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char*)sta_cfg.sta.password, cfg->sta_password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA started, connecting to SSID: %s, PASS: %s, scan_method: %d, sort_method: %d, PMF capable: %d, PMF required: %d",cfg->sta_ssid,cfg->sta_password, sta_cfg.sta.scan_method,sta_cfg.sta.sort_method,sta_cfg.sta.pmf_cfg.capable, sta_cfg.sta.pmf_cfg.required);
}

// --- Инициализация AP с статическим IP ---
void init_wifi_ap(const wifi_settings_t *cfg)
{
    ESP_LOGI(TAG, "Initializing AP mode...");

    check_and_stop_wifi();

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    assert(netif);

    // Настройка статического IP для AP
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton("192.168.4.1");
    ip_info.gw.addr = esp_ip4addr_aton("192.168.4.1");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    // Регистрация обработчиков для AP
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &wifi_event_handler, NULL));

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char*)ap_cfg.ap.password, cfg->ap_password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
    ap_cfg.ap.channel = cfg->ap_channel > 0 ? cfg->ap_channel : 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(cfg->ap_password) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started, SSID: %s, IP: 192.168.4.1", cfg->ap_ssid);
}



    // --- Инициализация Wi-Fi в зависимости от режима ---
    esp_err_t wifi_manager_init(const wifi_settings_t *cfg)
    {
        if (!wifi_evt_group) {
            wifi_evt_group = xEventGroupCreate();
            assert(wifi_evt_group);
        }

        if (ap_list_mutex == NULL) {
            ap_list_mutex = xSemaphoreCreateMutex();
        }

        if (cfg->mode == WIFI_MODE_STA) {
            init_wifi_sta(cfg);
        } else if (cfg->mode == WIFI_MODE_AP) {
            init_wifi_ap(cfg);
        } else if (cfg->mode == WIFI_MODE_APSTA) {
            // APSTA можно запускать последовательно: сначала AP, затем STA
            init_wifi_ap(cfg);
            init_wifi_sta(cfg);
        }

        ESP_LOGI(TAG, "Wi-Fi manager initialized in mode %d", cfg->mode);
        return ESP_OK;
    }

// --- Обработчик событий Wi-Fi ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch(event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn = event_data;
                ESP_LOGW(TAG, "STA disconnected, reason=%d", disconn->reason);
                esp_wifi_connect();
                break;
            }
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = event_data;
                ESP_LOGI(TAG, "Client connected to AP. MAC: %02x:%02x:%02x:%02x:%02x:%02x",event->mac[0], event->mac[1], event->mac[2],event->mac[3], event->mac[4], event->mac[5]);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event_disc = event_data;
                ESP_LOGI(TAG, "Client disconnected from AP. MAC: %02x:%02x:%02x:%02x:%02x:%02x",event_disc->mac[0], event_disc->mac[1], event_disc->mac[2], event_disc->mac[3], event_disc->mac[4], event_disc->mac[5]);
                break;
            }

           case WIFI_EVENT_SCAN_DONE: {
                uint16_t count = 0;
                ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&count));
                if (count > MAX_APs) count = MAX_APs;

                if (ap_list_mutex) xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(100));
                ap_count = count;
                ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&count, ap_list));

                ESP_LOGI(TAG, "Wi-Fi scan done. %d APs found", ap_count);

                // --- Детальный вывод списка AP ---
                cJSON *json_root = cJSON_CreateObject();
                cJSON *json_aps = cJSON_CreateArray();

                for (int i = 0; i < ap_count; i++) {
                    wifi_ap_record_t *ap = &ap_list[i];

                    const char *authmode_str;
                    switch (ap->authmode) {
                        case WIFI_AUTH_OPEN: authmode_str = "OPEN"; break;
                        case WIFI_AUTH_WEP: authmode_str = "WEP"; break;
                        case WIFI_AUTH_WPA_PSK: authmode_str = "WPA_PSK"; break;
                        case WIFI_AUTH_WPA2_PSK: authmode_str = "WPA2_PSK"; break;
                        case WIFI_AUTH_WPA_WPA2_PSK: authmode_str = "WPA_WPA2_PSK"; break;
                        case WIFI_AUTH_WPA3_PSK: authmode_str = "WPA3_PSK"; break;
                        case WIFI_AUTH_WPA2_WPA3_PSK: authmode_str = "WPA2_WPA3_PSK"; break;
                        default: authmode_str = "UNKNOWN"; break;
                    }

                    ESP_LOGI(TAG, "[%d] SSID: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %s",
                            i,
                            ap->ssid,
                            ap->bssid[0], ap->bssid[1], ap->bssid[2],
                            ap->bssid[3], ap->bssid[4], ap->bssid[5],
                            ap->rssi,
                            ap->primary,
                            authmode_str);

                    // --- Формируем JSON для AP ---
                    cJSON *ap_obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(ap_obj, "ssid", (const char*)ap->ssid);
                    char bssid_str[18];
                    snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x", ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
                    cJSON_AddStringToObject(ap_obj, "bssid", bssid_str);
                    cJSON_AddNumberToObject(ap_obj, "rssi", ap->rssi);
                    cJSON_AddNumberToObject(ap_obj, "channel", ap->primary);
                    cJSON_AddStringToObject(ap_obj, "authmode", authmode_str);

                    cJSON_AddItemToArray(json_aps, ap_obj);
                }

                if (ap_list_mutex) xSemaphoreGive(ap_list_mutex);

                cJSON_AddItemToObject(json_root, "type", cJSON_CreateString("wifi_scan_result"));
                cJSON_AddItemToObject(json_root, "aps", json_aps);

                char *json_str = cJSON_PrintUnformatted(json_root);
                if (json_str) {
                    ws_broadcast(json_str);  // Отправка всем клиентам WS
                    cJSON_free(json_str);
                }
                cJSON_Delete(json_root);

                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
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
    if (count) {
        if (ap_list_mutex) xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(100));
        *count = ap_count;
        if (ap_list_mutex) xSemaphoreGive(ap_list_mutex);
    }
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





 void wifi_manager_task(void *arg)
{
    wifi_settings_t cfg;

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            wifi_evt_group,
            WIFI_EVT_APPLY,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & WIFI_EVT_APPLY) {
            ESP_LOGI(TAG, "Applying new Wi-Fi settings…");

            nvs_load_wifi_settings(&cfg);
            wifi_manager_init(&cfg);
        }
    }
}


esp_err_t wifi_manager_request_apply(void)
{
    if (!wifi_evt_group) {
        ESP_LOGE(TAG, "wifi_evt_group not initialized");
        return ESP_FAIL;
    }

    xEventGroupSetBits(wifi_evt_group, WIFI_EVT_APPLY);
    ESP_LOGI(TAG, "Wi-Fi apply requested");
    return ESP_OK;
}