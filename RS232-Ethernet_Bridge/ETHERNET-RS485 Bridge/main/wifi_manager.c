

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "ws_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

#define MAX_APs 20
static const char *TAG = "wifi_manager";
static wifi_ap_record_t ap_list[MAX_APs];
static uint16_t ap_count = 0;

extern void ws_broadcast(const char *text);
extern void ws_send(int client_fd, const char *text);

static SemaphoreHandle_t ap_list_mutex = NULL;


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


// --- Событийный обработчик ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,int32_t event_id, void *event_data)
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
          wifi_event_sta_disconnected_t *disconn = event_data;
        ESP_LOGW("wifi", "Disconnected, reason=%d (%s)", disconn->reason, wifi_reason_to_str(disconn->reason));
        esp_wifi_connect();
        break;
    case WIFI_EVENT_SCAN_DONE:
      
        ESP_LOGI(TAG, "Scan done");
        uint16_t count = MAX_APs;
        // Получаем записи локально
        esp_err_t res = esp_wifi_scan_get_ap_records(&count, ap_list);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(res));
            break;
        }

        // Защитим апдейт
        if (ap_list_mutex) xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(100));
        ap_count = count;
        if (ap_list_mutex) xSemaphoreGive(ap_list_mutex);

        ESP_LOGI(TAG, "Found %d APs:", ap_count);

        // Формируем JSON вручную (чтобы не требовать cJSON в этом модуле).
        // Структура: {"type":"wifi_scan","status":"done","count":n,"aps":[{ssid, rssi, auth, channel}, ...]}
        char *buf = heap_caps_malloc(4096, MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGW(TAG, "no memory for scan JSON");
            break;
        }
        size_t pos = 0;
        pos += snprintf(buf + pos, 4090 - pos, "{\"type\":\"wifi_scan\",\"status\":\"done\",\"count\":%d,\"aps\":[", ap_count);

        for (int i = 0; i < ap_count && pos < 4090; i++) {
            // Берём безопасно локальный снимок записи (чтоб не читать во время апдейта)
            wifi_ap_record_t rec;
            if (ap_list_mutex) xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(100));
            rec = ap_list[i];
            if (ap_list_mutex) xSemaphoreGive(ap_list_mutex);

            // экранирование SSID не реализовано полностью для редких символов — можно улучшить
            pos += snprintf(buf + pos, 4090 - pos,
                            "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"channel\":%d}%s",
                            rec.ssid, rec.rssi, rec.authmode, rec.primary,
                            (i + 1 < ap_count) ? "," : "");
        }
        pos += snprintf(buf + pos, 4090 - pos, "]}");

        // Шлём всем WS клиентам (если ws_broadcast доступен)
        ws_broadcast(buf);

        heap_caps_free(buf);

        break;
      

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
    strncpy((char *)sta_cfg.sta.ssid, cfg->sta_ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, cfg->sta_password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.channel = 0;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // --- ВАЖНО: улучшенные настройки сканирования ---
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;             // полное сканирование всех каналов
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;         // выбирать AP с максимальным уровнем сигнала
    sta_cfg.sta.pmf_cfg.capable = true;                          // поддерживает PMF
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, cfg->ap_password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
    ap_cfg.ap.channel = cfg->ap_channel > 0 ? cfg->ap_channel : 1; // минимальный 1
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(cfg->ap_password) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;


    ESP_ERROR_CHECK(esp_wifi_set_mode(cfg->mode));

        if (cfg->mode == WIFI_MODE_STA) {
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        }
        else if (cfg->mode == WIFI_MODE_AP) {
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        }
        else if (cfg->mode == WIFI_MODE_APSTA) {
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        }



    ESP_ERROR_CHECK(esp_wifi_start());


     if (ap_list_mutex == NULL) {
        ap_list_mutex = xSemaphoreCreateMutex();
        if (ap_list_mutex == NULL) {
            ESP_LOGW(TAG, "Failed to create ap_list_mutex");
        }
    }
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








