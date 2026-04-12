


#include "wifi_manager.h"
#include "esp_wifi.h"
#include "ws_server.h"
#include "dns_server.h"
#include "network_state.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define MAX_APs 20
#define SCAN_RESULT_TASK_STACK_SIZE 8192
#define RECONNECT_TASK_STACK_SIZE 4096

static const char *TAG = "wifi_manager";
static wifi_ap_record_t ap_list[MAX_APs];
static uint16_t ap_count = 0;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif  = NULL;
static bool wifi_initialized = false;

static volatile bool wifi_scan_in_progress = false;
static volatile bool wifi_scan_completed = false;

// Сохранённые результаты сканирования
static char *pending_scan_results = NULL;
static SemaphoreHandle_t pending_results_mutex = NULL;

static EventGroupHandle_t wifi_evt_group;
#define WIFI_EVT_APPLY   BIT0
void send_pending_scan_results(void);
static void wifi_apply_ip_settings(const network_settings_t *cfg);
extern void ws_broadcast(const char *text);
extern void ws_send(int client_fd, const char *text);

static SemaphoreHandle_t ap_list_mutex = NULL;

// Структура для передачи данных сканирования в отдельную задачу
typedef struct {
    wifi_ap_record_t aps[MAX_APs];
    uint16_t count;
} scan_results_t;

// --- Forward ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void process_scan_results_task(void *pvParameters);
static void delayed_reconnect_task(void *pvParameters);
static void save_scan_results_for_later(const char *json_results);
static void reconnect_after_scan_task(void *pvParameters);
esp_err_t wifi_connect_to(const char* ssid, const char* password);
static const char* wifi_reason_to_str(uint8_t reason)
{
    switch (reason) {
        case 0: return "0: Unspecified";
        case 1: return "1: Auth expired";
        case 2: return "2: Auth leave";
        case 3: return "3: Assoc expired";
        case 4: return "4: Assoc too many";
        case 5: return "5: Not authed";
        case 6: return "6: Not assoced";
        case 7: return "7: Assoc leave";
        case 8: return "8: Assoc not authed";
        case 9: return "9: Disassoc pwrcap bad";
        case 10: return "10: Disassoc supchan bad";
        case 11: return "11: BSS transition";
        case 12: return "12: Disassoc unspecified";
        case 13: return "13: IE invalid";
        case 14: return "14: MIC failure";
        case 15: return "15: 4-way timeout";
        case 16: return "16: 4-way timeout";
        case 17: return "17: Group key timeout";
        case 18: return "18: IE differs";
        case 19: return "19: Group cipher invalid";
        case 20: return "20: Pairwise cipher invalid";
        case 21: return "21: AKMP invalid";
        case 22: return "22: RSN IE version";
        case 23: return "23: RSN capabilities";
        case 24: return "24: 802.1X failed";
        case 25: return "25: Cipher rejected";
        case 26: return "26: TSN timeout";
        case 27: return "27: WPA failed";
        case 28: return "28: DHCP failure";
        case 29: return "29: Beacon timeout";
        case 205: return "205: WPA failed (ESP32)";
        default: {
            static char buf[64];
            snprintf(buf, sizeof(buf), "Unknown %d", reason);
            return buf;
        }
    }
}

void wifi_manager_init_once(void)
{
    static bool inited = false;
    if (inited) return;
    
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();
    assert(sta_netif);
    assert(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());

    inited = true;
}

void wifi_apply_settings(const wifi_settings_t *cfg)
{
    ESP_LOGI(TAG, "Applying Wi-Fi settings, mode=%d", cfg->mode);

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(esp_wifi_set_mode(cfg->mode));

    if (cfg->mode == WIFI_MODE_STA || cfg->mode == WIFI_MODE_APSTA) {
        wifi_config_t sta_cfg = {0};
        strncpy((char*)sta_cfg.sta.ssid, cfg->sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char*)sta_cfg.sta.password, cfg->sta_password, sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.ssid[sizeof(sta_cfg.sta.ssid) - 1] = '\0';
        sta_cfg.sta.password[sizeof(sta_cfg.sta.password) - 1] = '\0';

        size_t pass_len = strlen(cfg->sta_password);
        if (pass_len == 0) {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        } else if (pass_len >= 8 && pass_len <= 63) {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }

        sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
        sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.failure_retry_cnt = 5;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_LOGI(TAG, "STA config applied: SSID=%s", cfg->sta_ssid);
    }

    if (cfg->mode == WIFI_MODE_AP || cfg->mode == WIFI_MODE_APSTA) {
        wifi_config_t ap_cfg = {0};
        strncpy((char*)ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        strncpy((char*)ap_cfg.ap.password, cfg->ap_password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.ssid[sizeof(ap_cfg.ap.ssid) - 1] = '\0';
        ap_cfg.ap.password[sizeof(ap_cfg.ap.password) - 1] = '\0';
        ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
        ap_cfg.ap.channel = cfg->ap_channel ? cfg->ap_channel : 1;
        ap_cfg.ap.max_connection = 4;

        size_t pass_len = strlen(cfg->ap_password);
        if (pass_len == 0) {
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        } else if (pass_len >= 8 && pass_len <= 63) {
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ESP_LOGW(TAG, "AP password too short, forcing OPEN");
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
            ap_cfg.ap.password[0] = '\0';
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_LOGI(TAG, "AP config applied: SSID=%s", cfg->ap_ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_apply_ip_settings(&net_cfg);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Wi-Fi started successfully");
}

esp_err_t wifi_manager_init(const wifi_settings_t *cfg)
{
    wifi_manager_init_once();
    wifi_apply_settings(cfg);
    wifi_apply_ip_settings(&net_cfg);
    return ESP_OK;
}

// --- Сохранение результатов для отложенной отправки ---
static void save_scan_results_for_later(const char *json_results) {
    if (pending_results_mutex) {
        xSemaphoreTake(pending_results_mutex, portMAX_DELAY);
        
        if (pending_scan_results) {
            free(pending_scan_results);
            pending_scan_results = NULL;
        }
        
        pending_scan_results = strdup(json_results);
        ESP_LOGI(TAG, "Scan results saved for later delivery");
        
        xSemaphoreGive(pending_results_mutex);
    }
}

void send_pending_scan_results(void) {
    if (pending_results_mutex && pending_scan_results) {
        xSemaphoreTake(pending_results_mutex, portMAX_DELAY);
        
        if (pending_scan_results) {
            ESP_LOGI(TAG, "Sending pending scan results after reconnection");
            ws_broadcast(pending_scan_results);
          //  free(pending_scan_results);
          //  pending_scan_results = NULL;
        }
        
        xSemaphoreGive(pending_results_mutex);
    }
}

// --- ОТДЕЛЬНАЯ ЗАДАЧА для обработки результатов сканирования ---
static void process_scan_results_task(void *pvParameters)
{
    scan_results_t *results = (scan_results_t *)pvParameters;
    
    if (!results) {
        ESP_LOGE(TAG, "process_scan_results_task: NULL results");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Processing %d scan results", results->count);

    // Копируем в глобальный список
    if (ap_list_mutex) {
        if (xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ap_count = results->count;
            memcpy(ap_list, results->aps, sizeof(wifi_ap_record_t) * results->count);
            xSemaphoreGive(ap_list_mutex);
        }
    }

    // Формируем JSON
    uint16_t send_count = results->count;
    if (send_count > 15) send_count = 15;

    cJSON *json_root = cJSON_CreateObject();
    if (json_root) {
        cJSON_AddStringToObject(json_root, "type", "wifi_scan_result");
        cJSON *json_aps = cJSON_CreateArray();
        
        for (int i = 0; i < send_count; i++) {
            wifi_ap_record_t *ap = &results->aps[i];

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

            cJSON *ap_obj = cJSON_CreateObject();
            if (ap_obj) {
                cJSON_AddStringToObject(ap_obj, "ssid", (const char*)ap->ssid);
                char bssid_str[18];
                snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x", 
                         ap->bssid[0], ap->bssid[1], ap->bssid[2],
                         ap->bssid[3], ap->bssid[4], ap->bssid[5]);
                cJSON_AddStringToObject(ap_obj, "bssid", bssid_str);
                cJSON_AddNumberToObject(ap_obj, "rssi", ap->rssi);
                cJSON_AddNumberToObject(ap_obj, "channel", ap->primary);
                cJSON_AddStringToObject(ap_obj, "authmode", authmode_str);
                cJSON_AddItemToArray(json_aps, ap_obj);
            }
        }
        cJSON_AddItemToObject(json_root, "aps", json_aps);
    }

    if (json_root) {
        char *json_str = cJSON_PrintUnformatted(json_root);
        if (json_str) {
            ESP_LOGI(TAG, "Preparing scan results (%d APs)", send_count);
            
            // Сохраняем результаты (будут отправлены после восстановления соединения)
            save_scan_results_for_later(json_str);
            
            cJSON_free(json_str);
        }
        cJSON_Delete(json_root);
    }

    wifi_scan_completed = true;
    free(results);
    ESP_LOGI(TAG, "Scan results processing finished");
    vTaskDelete(NULL);
}

// --- ОТДЕЛЬНАЯ ЗАДАЧА для восстановления WiFi ---
static void reconnect_to_saved_wifi_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "Reconnecting to saved WiFi");
    
    wifi_settings_t cfg;
    if (nvs_load_wifi_settings(&cfg) == ESP_OK && strlen(cfg.sta_ssid) > 0) {
        wifi_connect_to(cfg.sta_ssid, cfg.sta_password);
    }
    
    vTaskDelete(NULL);
}

// --- Обработчик событий Wi-Fi ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch(event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                esp_wifi_connect();
                network_set_wifi_state(NET_STATE_WIFI_CONNECTING);
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected");
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn = event_data;
                ESP_LOGW(TAG, "STA disconnected, reason=%d (%s)", 
                         disconn->reason, wifi_reason_to_str(disconn->reason));
                network_set_wifi_state(NET_STATE_WIFI_DOWN);
                
                // Если это не сканирование - переподключаемся
                if (!wifi_scan_in_progress) {
                    xTaskCreate(reconnect_to_saved_wifi_task, "reconnect_wifi", 
                               RECONNECT_TASK_STACK_SIZE, NULL, 3, NULL);
                }
                break;
            }
            
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                dns_start();
                break;
                
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                dns_stop();
                ws_cleanup_all_clients();
                break;
            
            /*    
            case WIFI_EVENT_AP_STACONNECTED:
            case WIFI_EVENT_AP_STADISCONNECTED:
             ws_cleanup_all_clients();
                break;
            */

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *conn = event_data;
                
                // Ручное форматирование MAC адреса
                ESP_LOGI(TAG, "Station connected, MAC=%02x:%02x:%02x:%02x:%02x:%02x, AID=%d",
                        conn->mac[0], conn->mac[1], conn->mac[2],
                        conn->mac[3], conn->mac[4], conn->mac[5], conn->aid);
                break;
            }
                
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *disconn = event_data;
                
                ESP_LOGI(TAG, "Station disconnected, MAC=%02x:%02x:%02x:%02x:%02x:%02x, reason=%d",
                        disconn->mac[0], disconn->mac[1], disconn->mac[2],
                        disconn->mac[3], disconn->mac[4], disconn->mac[5], disconn->reason);

                ws_notify_reconnect();
                dns_stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                dns_start();
                ws_cleanup_all_clients();
                break;
            }

            case WIFI_EVENT_SCAN_DONE: {
                ESP_LOGI(TAG, "SCAN_DONE event received");
                wifi_scan_in_progress = false;
                
                uint16_t count = 0;
                esp_wifi_scan_get_ap_num(&count);
                ESP_LOGI(TAG, "Found %d APs", count);
                
                if (count == 0) {
                    ESP_LOGI(TAG, "No APs found");
                    break;
                }
                
                if (count > MAX_APs) count = MAX_APs;
                
                scan_results_t *results = (scan_results_t *)malloc(sizeof(scan_results_t));
                if (!results) {
                    ESP_LOGE(TAG, "Failed to allocate");
                    break;
                }
                
                results->count = count;
                esp_err_t err = esp_wifi_scan_get_ap_records(&count, results->aps);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to get records: %s", esp_err_to_name(err));
                    free(results);
                    break;
                }
                
                // Запускаем обработку результатов
                if (xTaskCreate(process_scan_results_task, "scan_proc", 
                               SCAN_RESULT_TASK_STACK_SIZE, results, 2, NULL) != pdTRUE) {
                    ESP_LOGE(TAG, "Failed to create scan task");
                    free(results);
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*)event_data;
        network_set_wifi_state(NET_STATE_WIFI_UP);
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Отправляем сохранённые результаты сканирования
        if (wifi_scan_completed) {
            send_pending_scan_results();
            wifi_scan_completed = false;
        }
    }
}

// --- Сканирование ---
esp_err_t wifi_scan_networks(void)
{
    ESP_LOGI(TAG, "wifi_scan_networks called");
    
    if (wifi_scan_in_progress) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_in_progress = true;
    wifi_scan_completed = false;
    
    // Очищаем старые результаты
    if (pending_results_mutex) {
        xSemaphoreTake(pending_results_mutex, portMAX_DELAY);
        if (pending_scan_results) {
            free(pending_scan_results);
            pending_scan_results = NULL;
        }
        xSemaphoreGive(pending_results_mutex);
    }
    
    // Отправляем статус старта
    ws_broadcast("{\"type\":\"wifi_scan\",\"status\":\"started\"}");
    vTaskDelay(pdMS_TO_TICKS(50));
    

     // Проверяем режим WiFi
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    ESP_LOGI(TAG, "Current WiFi mode: %d", mode);

    if (mode != WIFI_MODE_AP) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (mode == WIFI_MODE_AP) {
    ESP_LOGI(TAG, "Switching AP -> APSTA for scan");
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    vTaskDelay(pdMS_TO_TICKS(200)); // важно!
}
    
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
       // .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            }
        }
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
        wifi_scan_in_progress = false;
        return err;
    }

    // ЗАПУСКАЕМ ЗАДАЧУ ВОССТАНОВЛЕНИЯ ПОСЛЕ СКАНИРОВАНИЯ
    xTaskCreate(reconnect_after_scan_task, "reconnect_after_scan", 
               RECONNECT_TASK_STACK_SIZE, NULL, 3, NULL);

    ESP_LOGI(TAG, "Scan started");
    return ESP_OK;
}


// --- НОВАЯ ЗАДАЧА для восстановления после сканирования ---
static void reconnect_after_scan_task(void *pvParameters)
{
    // Ждём завершения сканирования (максимум 10 секунд)
    int wait_count = 0;
    while (wifi_scan_in_progress && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
    
    // Дополнительная задержка перед reconnect
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "Restoring WiFi connection after scan");
    
    // Загружаем сохранённые настройки WiFi
    wifi_settings_t cfg;
    if (nvs_load_wifi_settings(&cfg) == ESP_OK && strlen(cfg.sta_ssid) > 0) {
        ESP_LOGI(TAG, "Reconnecting to saved WiFi: %s", cfg.sta_ssid);
        wifi_connect_to(cfg.sta_ssid, cfg.sta_password);
    } else {
        ESP_LOGW(TAG, "No saved WiFi credentials, AP mode already active");
        // AP режим уже работает, ничего не делаем
    }
    
    vTaskDelete(NULL);
}



// --- Получение списка AP ---
const wifi_ap_record_t* wifi_get_ap_list(uint16_t *count)
{
    if (count) {
        if (ap_list_mutex && xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            *count = ap_count;
            xSemaphoreGive(ap_list_mutex);
        } else {
            *count = ap_count;
        }
    }
    return ap_list;
}

// --- Подключение к WiFi ---
esp_err_t wifi_connect_to(const char* ssid, const char* password)
{
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    sta_cfg.sta.ssid[sizeof(sta_cfg.sta.ssid) - 1] = '\0';

    size_t pass_len = password ? strlen(password) : 0;
    if (pass_len >= 8 && pass_len <= 63) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.password[sizeof(sta_cfg.sta.password) - 1] = '\0';
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.password[0] = '\0';
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    return ESP_OK;
}

void wifi_manager_stop(void)
{
    wifi_scan_in_progress = false;
    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi stopped");
}

void wifi_manager_task(void *arg)
{
    wifi_settings_t cfg;
    
    for (;;) {
        xEventGroupWaitBits(wifi_evt_group, WIFI_EVT_APPLY, pdTRUE, pdFALSE, portMAX_DELAY);
        ESP_LOGI(TAG, "Apply requested");
        if (nvs_load_wifi_settings(&cfg) == ESP_OK) {
            wifi_manager_init(&cfg);
        }
    }
}

esp_err_t wifi_manager_request_apply(void)
{
    if (!wifi_evt_group) return ESP_FAIL;
    xEventGroupSetBits(wifi_evt_group, WIFI_EVT_APPLY);
    return ESP_OK;
}

void start_wifi_manager_task(void)
{
    if (!wifi_evt_group) {
        wifi_evt_group = xEventGroupCreate();
        assert(wifi_evt_group);
    }

    if (!ap_list_mutex) {
        ap_list_mutex = xSemaphoreCreateMutex();
        assert(ap_list_mutex);
    }
    
    if (!pending_results_mutex) {
        pending_results_mutex = xSemaphoreCreateMutex();
        assert(pending_results_mutex);
    }

    xTaskCreatePinnedToCore(wifi_manager_task, "wifi_manager_task", 4096, 
                           NULL, 3, NULL, tskNO_AFFINITY);
    
    ESP_LOGI(TAG, "Wi-Fi manager started");
}

static void wifi_apply_ip_settings(const network_settings_t *cfg)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "STA netif not found");
        return;
    }

    if (cfg->wifi_dhcp_enabled) {
        esp_netif_dhcpc_start(netif);
        return;
    }

    esp_netif_dhcpc_stop(netif);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));

    ip.ip.addr = esp_ip4addr_aton(cfg->wifi_ip);
    ip.netmask.addr = esp_ip4addr_aton(cfg->wifi_mask);
    ip.gw.addr = esp_ip4addr_aton(cfg->wifi_gateway);

    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    if (strlen(cfg->wifi_dns)) {
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(cfg->wifi_dns);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
}

