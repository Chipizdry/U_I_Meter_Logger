

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "ws_server.h"
#include "dns_server.h"
#include "network_state.h"
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

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif  = NULL;
static bool wifi_initialized = false;

static volatile bool wifi_scan_in_progress = false;


static EventGroupHandle_t wifi_evt_group;
#define WIFI_EVT_APPLY   BIT0
static void wifi_stop_connecting_for_scan(void);
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
    //esp_wifi_deinit();
    ESP_LOGI(TAG, "Wi-Fi deinitialized");
}


static const char* wifi_reason_to_str(uint8_t reason)
{
    switch (reason) {
        // --- Стандартные коды ESP-IDF ---
        case 0: return "0: Unspecified / Unknown reason";
        case 1: return "1: Auth expired (deauthenticated because auth timed out)";
        case 2: return "2: Auth leave (station leaving network)";
        case 3: return "3: Assoc expired (association expired)";
        case 4: return "4: Assoc too many (AP reached max assoc limit)";
        case 5: return "5: Not authed (station not authenticated)";
        case 6: return "6: Not assoced (station not associated)";
        case 7: return "7: Assoc leave (station leaving network)";
        case 8: return "8: Assoc not authed (association without auth)";
        case 9: return "9: Disassoc pwrcap bad (power capability mismatch)";
        case 10: return "10: Disassoc supchan bad (unsupported channel)";
        case 11: return "11: BSS transition management not supported";
        case 12: return "12: Disassoc unspecified reason";
        case 13: return "13: IE invalid (invalid information element in frame)";
        case 14: return "14: MIC failure (Michael MIC failure)";
        case 15: return "15: 4-way handshake timeout (common WPA2 failure)";
        case 16: return "16: 4-way handshake timeout";
        case 17: return "17: Group key update timeout";
        case 18: return "18: IE in 4-way differs (mismatch in handshake)";
        case 19: return "19: Group cipher invalid";
        case 20: return "20: Pairwise cipher invalid";
        case 21: return "21: AKMP invalid (authentication key management problem)";
        case 22: return "22: Unsupported RSN IE version";
        case 23: return "23: Invalid RSN IE capabilities";
        case 24: return "24: 802.1X authentication failed";
        case 25: return "25: Cipher suite rejected";

        // --- Часто встречающиеся нестандартные / ESP32 внутренние ---
        case 26: return "26: TSN timeout / proprietary reason";
        case 27: return "27: WPA handshake failed / proprietary reason";
        case 28: return "28: DHCP failure";
        case 29: return "29: Beacon timeout";
        case 205: return "205: WPA handshake failed / proprietary ESP32 internal";

        // --- Любые неизвестные коды ---
        default: {
            static char buf[64];
            snprintf(buf, sizeof(buf), "Unknown reason %d", reason);
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
    ESP_LOGI(TAG, "Applying Wi-Fi settings safely, mode=%d", cfg->mode);

    // 1️⃣ Останавливаем Wi-Fi, игнорируем ошибки если уже остановлен
    esp_wifi_stop();

    // 2️⃣ Выбираем нужный режим
    ESP_ERROR_CHECK(esp_wifi_set_mode(cfg->mode));

    // 3️⃣ Применяем конфигурацию STA (если режим STA или APSTA)
    if (cfg->mode == WIFI_MODE_STA || cfg->mode == WIFI_MODE_APSTA) {
        wifi_config_t sta_cfg = {0};
        strncpy((char*)sta_cfg.sta.ssid, cfg->sta_ssid, sizeof(sta_cfg.sta.ssid));
        strncpy((char*)sta_cfg.sta.password, cfg->sta_password, sizeof(sta_cfg.sta.password));

         // ЯВНО УСТАНАВЛИВАЕМ threshold.authmode чтобы избежать ворнинга
        size_t pass_len = strlen(cfg->sta_password);
        if (pass_len == 0) {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        } else if (pass_len >= 8 && pass_len <= 63) {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            // Для WEP или других нестандартных случаев
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }

        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_LOGI(TAG, "STA config applied: SSID=%s", cfg->sta_ssid);
    }

    // 4️⃣ Применяем конфигурацию AP (если режим AP или APSTA)
    if (cfg->mode == WIFI_MODE_AP || cfg->mode == WIFI_MODE_APSTA) {
        wifi_config_t ap_cfg = {0};
        strncpy((char*)ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
        strncpy((char*)ap_cfg.ap.password, cfg->ap_password, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
        ap_cfg.ap.channel = cfg->ap_channel ? cfg->ap_channel : 1;
        ap_cfg.ap.max_connection = 4;

          // Устанавливаем authmode для AP
        size_t pass_len = strlen(cfg->ap_password);
        if (pass_len == 0) {
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        } else if (pass_len >= 8 && pass_len <= 63) {
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            // Пароль слишком короткий - принудительно OPEN
            ESP_LOGW(TAG, "AP password too short (%d), forcing OPEN auth", pass_len);
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
            ap_cfg.ap.password[0] = '\0';
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_LOGI(TAG, "AP config applied: SSID=%s", cfg->ap_ssid);
    }

    // 5️⃣ Запускаем Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi started successfully in mode %d", cfg->mode);
}


esp_err_t wifi_manager_init(const wifi_settings_t *cfg)
{
    wifi_manager_init_once();
    wifi_apply_settings(cfg);
    return ESP_OK;
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
    //sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
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
   // ap_cfg.ap.authmode = strlen(cfg->ap_password) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    size_t pass_len = strlen(cfg->ap_password);

    if (pass_len == 0) {
        // Открытая точка
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.password[0] = '\0';
    } 
    else if (pass_len < 8) {
        // Невалидный пароль для WPA — принудительно OPEN
        ESP_LOGW(TAG, "AP password too short (%d), forcing OPEN auth", pass_len);
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.password[0] = '\0';
    } 
    else {
        // Явно WPA2, без WPA1 и без warning
        strncpy((char *)ap_cfg.ap.password, cfg->ap_password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started, SSID: %s, IP: 192.168.4.1", cfg->ap_ssid);
}


// --- Обработчик событий Wi-Fi ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch(event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
                network_set_wifi_state(NET_STATE_WIFI_CONNECTING);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn = event_data;
                 ESP_LOGW(TAG, "STA disconnected, reason=%d (%s)",disconn->reason, wifi_reason_to_str(disconn->reason));
                 network_set_wifi_state(NET_STATE_WIFI_DOWN);
               if (!wifi_scan_in_progress) {
                        esp_wifi_connect();
                    } else {
                        ESP_LOGI(TAG, "STA reconnect skipped due to scan");
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

                  wifi_scan_in_progress = false;

        

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
         network_set_wifi_state(NET_STATE_WIFI_UP);
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}



// --- Неблокирующее сканирование ---
esp_err_t wifi_scan_networks(void)
{

   if (wifi_scan_in_progress) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_stop_connecting_for_scan();



    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_scan skipped, err=%s", esp_err_to_name(err));
          wifi_scan_in_progress = false;
        return err;   
    }


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
  //  strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
  //  strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    memset(&sta_cfg, 0, sizeof(sta_cfg));

    // SSID
    strncpy((char *)sta_cfg.sta.ssid, ssid,
            sizeof(sta_cfg.sta.ssid) - 1);

    size_t pass_len = password ? strlen(password) : 0;
    memset(sta_cfg.sta.password, 0, sizeof(sta_cfg.sta.password));
    if (pass_len >= 8 && pass_len <= 63) {
        // WPA2
        strncpy((char *)sta_cfg.sta.password, password,
                sizeof(sta_cfg.sta.password) - 1);

        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        // OPEN (важно: пароль пустой)
        sta_cfg.sta.password[0] = '\0';
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return ESP_OK;
}

void wifi_manager_stop(void)
{
    esp_wifi_stop();
   // esp_wifi_deinit();
    ESP_LOGI(TAG, "Wi-Fi stopped");
}




void wifi_manager_task(void *arg)
{
    wifi_settings_t cfg;

    for (;;) {
        xEventGroupWaitBits(
            wifi_evt_group,
            WIFI_EVT_APPLY,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        ESP_LOGI(TAG, "Wi-Fi apply requested");
        nvs_load_wifi_settings(&cfg);
        wifi_manager_init(&cfg);
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


void start_wifi_manager_task(void)
{
    // 1️⃣ Создаем EventGroup, если не создан
    if (!wifi_evt_group) {
        wifi_evt_group = xEventGroupCreate();
        assert(wifi_evt_group);
    }

    // 2️⃣ Создаем mutex для списка AP
    if (!ap_list_mutex) {
        ap_list_mutex = xSemaphoreCreateMutex();
        assert(ap_list_mutex);
    }

    // 3️⃣ Создаем задачу менеджера Wi-Fi
    xTaskCreate(wifi_manager_task, "wifi_manager_task", 4096, NULL, 5, NULL);
}



static void wifi_stop_connecting_for_scan(void)
{
    wifi_scan_in_progress = true;

    esp_wifi_disconnect();

    ESP_LOGI(TAG, "STA connecting stopped for scan");
}



