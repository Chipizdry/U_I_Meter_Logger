


#include "web_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"  
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "nvs_settings.h"
#include "esp_random.h"
#include "cJSON.h" 
#include <string.h>
#include "ethernet_manager.h"
#include "lwip/inet.h" 
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include <stdbool.h>
#include "ota_update.h"
#include "websocket_client.h"
#include "rs485_master.h"
#include "ws_server.h"
#include "driver/uart.h"

static const char *TAG = "web_server";
extern char cloud_status_msg[32] ;   // статус по умолчанию
httpd_handle_t server = NULL;
char auth_token[64] = {0};
static void url_decode(char *dst, const char *src);

static const char* wifi_mode_to_string(wifi_mode_t mode) { 
    switch (mode) { case WIFI_MODE_NULL: return "WIFI_MODE_NULL";
        case WIFI_MODE_STA: return "WIFI_MODE_STA";
        case WIFI_MODE_AP: return "WIFI_MODE_AP"; 
        case WIFI_MODE_APSTA: return "WIFI_MODE_APSTA"; 
        case WIFI_MODE_MAX: return "WIFI_MODE_MAX"; 
        default: return "UNKNOWN"; } }

// === Генерация токена ===
static void generate_token(char *buf, size_t len)
{
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < len - 1; i++) {
        uint32_t rnd = esp_random() % (sizeof(charset) - 1);
        buf[i] = charset[rnd];
    }
    buf[len - 1] = '\0';
}

 bool check_token(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Checking token...");
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    ESP_LOGI(TAG, "Header len: %d", buf_len);
    ESP_LOGI(TAG, "Server auth_token: '%s'", auth_token);
    
    if (buf_len <= 1) return false;

    char *buf = malloc(buf_len);
    httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len);
    ESP_LOGI(TAG, "Header content: %s", buf);
    bool ok = false;
    if (strncmp(buf, "Bearer ", 7) == 0) {
        const char *client_token = buf + 7;
    ESP_LOGI(TAG, "Client token: '%s'", client_token);
        if (strcmp(client_token, auth_token) == 0) {
            ok = true;
        }
    }
    free(buf);
    return ok;
}




// === POST /login ===
static esp_err_t login_post_handler(httpd_req_t *req)
{
    char buf[256];
    int len = req->content_len;
    if (len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // --- Парсим form-urlencoded данные ---
    char input_login[64] = {0};
    char input_pass[64] = {0};
    sscanf(buf, "login=%63[^&]&password=%63s", input_login, input_pass);

    // --- Загружаем настройки пользователя из NVS ---
    user_settings_t user_settings = {0};
    esp_err_t err = nvs_load_user_settings(&user_settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load user settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load user settings");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loaded stored user: %s", user_settings.login);

    // --- Проверяем введённые данные ---
    if (strcmp(input_login, user_settings.login) == 0 &&
        strcmp(input_pass, user_settings.password) == 0)
    {
        generate_token(auth_token, sizeof(auth_token));
        ESP_LOGI(TAG, "User '%s' authenticated, token: %s", input_login, auth_token);

        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "token", auth_token);
        cJSON_AddNumberToObject(json, "build_number", sys.build_number);
        cJSON_AddStringToObject(json, "build_date", sys.build_date);
        const char *response = cJSON_PrintUnformatted(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
        cJSON_Delete(json);
        free((void*)response);
    } else {
        ESP_LOGW(TAG, "Invalid login attempt: %s / %s", input_login, input_pass);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid credentials 💩");
    }

    return ESP_OK;
}

/// GET /get_settings

static esp_err_t get_settings_handler(httpd_req_t *req)
{
    if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    // Определяем раздел прямо в локальном enum
    enum { ALL, UART, USER, NETWORK,WIFI, SYSTEM, UNKNOWN } section = ALL;

    const char *uri = req->uri;
    if      (strstr(uri, "/uart"))    section = UART;
    else if (strstr(uri, "/user"))    section = USER;
    else if (strstr(uri, "/network")) section = NETWORK;
    else if (strstr(uri, "/wifi")) section = WIFI;
    else if (strstr(uri, "/system"))  section = SYSTEM;
    else if (strstr(uri, "/get_settings")) section = ALL;
    else section = UNKNOWN;

    if (section == UNKNOWN) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown settings section");
        return ESP_FAIL;
    }

    // Загружаем настройки
    user_settings_t user = {0};
    network_settings_t net = {0};
    system_settings_t sys = {0};
    uart_settings_t uart_cfg = {0};
    wifi_settings_t wifi_cfg= {0};    

    nvs_load_user_settings(&user);
    nvs_load_network_settings(&net);
    nvs_load_system_settings(&sys);
    nvs_load_uart_settings(&uart_cfg);
    nvs_load_wifi_settings(&wifi_cfg);

    // Если DHCP включен, берем текущие настройки с Ethernet
    if (net.dhcp_enabled) {
        esp_netif_ip_info_t ip_info = ethernet_get_ip_info();
        char ip[16], mask[16], gw[16];
        inet_ntoa_r(ip_info.ip, ip, sizeof(ip));
        inet_ntoa_r(ip_info.netmask, mask, sizeof(mask));
        inet_ntoa_r(ip_info.gw, gw, sizeof(gw));

        strncpy(net.ip, ip, sizeof(net.ip));
        strncpy(net.mask, mask, sizeof(net.mask));
        strncpy(net.gateway, gw, sizeof(net.gateway));
        // DNS можно оставить как есть или брать с DHCP клиента

        esp_netif_t *eth_if = esp_netif_get_handle_from_ifkey("ETH_DEF"); // или ваш Ethernet интерфейс
        esp_netif_dns_info_t dns_info;
        
        if (esp_netif_get_dns_info(eth_if, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            inet_ntoa_r(dns_info.ip.u_addr.ip4, net.dns, sizeof(net.dns));
        } else {
            strcpy(net.dns, "0.0.0.0");
        }
    }


    // Сбор JSON
    cJSON *json = cJSON_CreateObject();

    switch (section) {

        case ALL:
        case USER: {
            cJSON *u = cJSON_CreateObject();
            cJSON_AddStringToObject(u, "login", user.login);
            cJSON_AddStringToObject(u, "account_login", user.account_login);  // ✅ имя аккаунта
            cJSON_AddStringToObject(u, "language", user.language);
            cJSON_AddStringToObject(u, "serial", user.serial);     // ✅ серийный номер      
            cJSON_AddBoolToObject(u, "connected", ws_connected); 
            cJSON_AddStringToObject(u, "status", cloud_status_msg);
            cJSON_AddItemToObject(json, "user", u);
            if (section != ALL) break;
            [[fallthrough]]; 
        }

        case NETWORK: {
            cJSON *n = cJSON_CreateObject();
            cJSON_AddStringToObject(n, "ip", net.ip);
            cJSON_AddStringToObject(n, "mask", net.mask);
            cJSON_AddStringToObject(n, "gateway", net.gateway);
            cJSON_AddStringToObject(n, "dns", net.dns); 
            cJSON_AddNumberToObject(n, "port", net.port);
            cJSON_AddBoolToObject(n, "dhcp_enabled", net.dhcp_enabled);
            cJSON_AddItemToObject(json, "network", n);
            if (section != ALL) break;
            [[fallthrough]]; 
        }

          case WIFI: {
            cJSON *w = cJSON_CreateObject();
            // STA
            cJSON_AddStringToObject(w, "sta_ssid", wifi_cfg.sta_ssid);
            cJSON_AddStringToObject(w, "sta_password", wifi_cfg.sta_password);
            // AP
            cJSON_AddStringToObject(w, "ap_ssid", wifi_cfg.ap_ssid);
            cJSON_AddStringToObject(w, "ap_password", wifi_cfg.ap_password);
            cJSON_AddNumberToObject(w, "ap_channel", wifi_cfg.ap_channel);
            // Режим WiFi
            cJSON_AddNumberToObject(w, "mode", wifi_cfg.mode);
            // IP / DHCP 
            cJSON_AddStringToObject(w, "ip", wifi_cfg.ip);
            cJSON_AddStringToObject(w, "mask", wifi_cfg.mask);
            cJSON_AddStringToObject(w, "gateway", wifi_cfg.gateway);
            cJSON_AddStringToObject(w, "dns", wifi_cfg.dns);
            cJSON_AddBoolToObject(w, "dhcp_enabled", wifi_cfg.dhcp_enabled);

            cJSON_AddItemToObject(json, "wifi", w);
            if (section != ALL) break;
            [[fallthrough]]; 
        }

        case SYSTEM: {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddNumberToObject(s, "refresh", sys.refresh_interval);
            cJSON_AddBoolToObject(s, "debug", sys.debug_mode);
            cJSON_AddNumberToObject(s, "build_number", sys.build_number);
            cJSON_AddStringToObject(s, "build_date", sys.build_date);
            cJSON_AddItemToObject(json, "system", s);
            if (section != ALL) break;
            [[fallthrough]]; 
        }

        case UART: {

            int real_data_bits = 8; // по умолчанию
            switch (uart_cfg.data_bits) {
                case UART_DATA_5_BITS: real_data_bits = 5; break;
                case UART_DATA_6_BITS: real_data_bits = 6; break;
                case UART_DATA_7_BITS: real_data_bits = 7; break;
                case UART_DATA_8_BITS: real_data_bits = 8; break;
            }
            
            int real_stop_bits = 1;
            switch (uart_cfg.stop_bits) {
                case UART_STOP_BITS_1:   real_stop_bits = 1; break;
                case UART_STOP_BITS_1_5: real_stop_bits = 15; break; // 1.5 бит
                case UART_STOP_BITS_2:   real_stop_bits = 2; break;
            }

            cJSON *u = cJSON_CreateObject();
            cJSON_AddNumberToObject(u, "baud", uart_cfg.baud_rate);
            cJSON_AddNumberToObject(u, "data_bits", real_data_bits);
            cJSON_AddNumberToObject(u, "stop_bits", real_stop_bits);
            cJSON_AddNumberToObject(u, "parity", uart_cfg.parity);
            cJSON_AddBoolToObject(u, "rs485_mode", uart_cfg.rs485_mode);
            cJSON_AddItemToObject(json, "uart", u);
            break;
        }

        default: break;
    }

    const char *resp = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    cJSON_Delete(json);
    free((void*)resp);

    return ESP_OK;
}







// ==== Отправка статического файла из /littlefs ====
static esp_err_t file_get_handler(httpd_req_t *req)
{

    // Обрабатывать только GET и HEAD
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET/HEAD only");
    }

    char filepath[128] = "/littlefs";

    // "/" → "/main.html"
    if (strcmp(req->uri, "/") == 0) {
        strcat(filepath, "/main.html");
    } else {
        strcat(filepath, req->uri);
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        return httpd_resp_send_404(req);
    }


    // MIME тип
    if (strstr(filepath, ".html"))
        httpd_resp_set_type(req, "text/html");
    else if (strstr(filepath, ".css"))
        httpd_resp_set_type(req, "text/css");
    else if (strstr(filepath, ".js"))
        httpd_resp_set_type(req, "application/javascript");
    else if (strstr(filepath, ".png"))
        httpd_resp_set_type(req, "image/png");
    else if (strstr(filepath, ".jpg"))
        httpd_resp_set_type(req, "image/jpeg");
    else
        httpd_resp_set_type(req, "text/plain");

    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, chunksize);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // завершить ответ

    ESP_LOGI(TAG, "Served file: %s", filepath);
    return ESP_OK;
}



/// POST /save_settings
static esp_err_t save_settings_post_handler(httpd_req_t *req)
{
    if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }


    const char *uri = req->uri;
    enum { SECTION_USER,SECTION_ACCOUNT, SECTION_NETWORK,SECTION_WIFI, SECTION_UART, SECTION_SYSTEM, SECTION_UNKNOWN } section = SECTION_UNKNOWN;

    if      (strstr(uri, "/save_settings/user"))    section = SECTION_USER;
    else if (strstr(uri, "/save_settings/account")) section = SECTION_ACCOUNT;
    else if (strstr(uri, "/save_settings/network")) section = SECTION_NETWORK;
    else if (strstr(uri, "/save_settings/wifi"))    section = SECTION_WIFI;
    else if (strstr(uri, "/save_settings/uart"))    section = SECTION_UART;
    else if (strstr(uri, "/save_settings/system"))  section = SECTION_SYSTEM;
    else section = SECTION_UNKNOWN;

    if (section == SECTION_UNKNOWN) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown settings section");
        return ESP_FAIL;
    }


    


    if (section == SECTION_UART) {
        char body[512] = {0};
        int ret = httpd_req_recv(req, body, sizeof(body)-1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
            return ESP_FAIL;
        }
        body[ret] = '\0';
    
        cJSON *json = cJSON_Parse(body);
        if (!json) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }
    
        uart_settings_t uart_cfg = {0};
        nvs_load_uart_settings(&uart_cfg);
       
       
        cJSON *baud = cJSON_GetObjectItem(json, "baud");
        cJSON *data_bits = cJSON_GetObjectItem(json, "data_bits");
        cJSON *stop_bits = cJSON_GetObjectItem(json, "stop_bits");
        cJSON *parity = cJSON_GetObjectItem(json, "parity");
        cJSON *mode = cJSON_GetObjectItem(json, "mode");
        if (baud && cJSON_IsNumber(baud)) uart_cfg.baud_rate = baud->valueint;
        if (data_bits && cJSON_IsNumber(data_bits)) {
            switch (data_bits->valueint) {
                case 5: uart_cfg.data_bits = UART_DATA_5_BITS; break;
                case 6: uart_cfg.data_bits = UART_DATA_6_BITS; break;
                case 7: uart_cfg.data_bits = UART_DATA_7_BITS; break;
                case 8: uart_cfg.data_bits = UART_DATA_8_BITS; break;
                default: uart_cfg.data_bits = UART_DATA_8_BITS; break;
            }
        }
        if (stop_bits && cJSON_IsNumber(stop_bits)) {
            switch (stop_bits->valueint) {
                case 1: uart_cfg.stop_bits = UART_STOP_BITS_1; break;
                case 2: uart_cfg.stop_bits = UART_STOP_BITS_2; break;
                case 15: uart_cfg.stop_bits = UART_STOP_BITS_1_5; break; // 1.5 -> 15
                default: uart_cfg.stop_bits = UART_STOP_BITS_1; break;
            }
        }
        if (parity && cJSON_IsNumber(parity)) {
            switch (parity->valueint) {
                case UART_PARITY_DISABLE:
                case UART_PARITY_EVEN:
                case UART_PARITY_ODD:
                    uart_cfg.parity = parity->valueint;
                    break;
                default:
                    uart_cfg.parity = UART_PARITY_DISABLE;
                    break;
            }
        }
        if (mode && cJSON_IsNumber(mode)) {
           uart_cfg.rs485_mode = mode->valueint;   // 0 = RS232, 1 = RS485
        }
        // --- Логгирование полученных настроек ---
        ESP_LOGI("UART_SETTINGS", "Received UART config: baud=%d, data_bits=%d, stop_bits=%d, parity=%d  ,mode=%s ",uart_cfg.baud_rate, uart_cfg.data_bits, uart_cfg.stop_bits, uart_cfg.parity, uart_cfg.rs485_mode ? "RS485" : "RS232");

        nvs_save_uart_settings(&uart_cfg);
        cJSON_Delete(json);
    
          // --- Применяем новые настройки сразу ---
        rs485_master_deinit();                              // останавливаем текущий UART/RS485 master
       
        rs485_master_init_from_cfg(&uart_cfg, 2048, 1024);  // инициализируем заново с новыми параметрами

        httpd_resp_sendstr(req, "UART settings saved successfully 💾");
        return ESP_OK;
    }

  if (section == SECTION_WIFI) {




    char body[512] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body)-1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    body[ret] = '\0';

    cJSON *json = cJSON_Parse(body);
     ESP_LOGI("WIFI", "Received WiFi JSON: %s", body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    wifi_settings_t wifi_cfg = {0};
    nvs_load_wifi_settings(&wifi_cfg);

    cJSON *sta_ssid = cJSON_GetObjectItem(json, "sta_ssid");
    cJSON *sta_password = cJSON_GetObjectItem(json, "sta_password");
    cJSON *ap_ssid = cJSON_GetObjectItem(json, "ap_ssid");
    cJSON *ap_password = cJSON_GetObjectItem(json, "ap_password");
    cJSON *ap_channel = cJSON_GetObjectItem(json, "ap_channel");
    cJSON *mode = cJSON_GetObjectItem(json, "mode");

    if (sta_ssid && cJSON_IsString(sta_ssid)) strncpy(wifi_cfg.sta_ssid, sta_ssid->valuestring, sizeof(wifi_cfg.sta_ssid) - 1);
    if (sta_password && cJSON_IsString(sta_password)) strncpy(wifi_cfg.sta_password, sta_password->valuestring, sizeof(wifi_cfg.sta_password) - 1);
    if (ap_ssid && cJSON_IsString(ap_ssid)) strncpy(wifi_cfg.ap_ssid, ap_ssid->valuestring, sizeof(wifi_cfg.ap_ssid) - 1);
    if (ap_password && cJSON_IsString(ap_password)) strncpy(wifi_cfg.ap_password, ap_password->valuestring, sizeof(wifi_cfg.ap_password) - 1);
    if (ap_channel && cJSON_IsNumber(ap_channel)) wifi_cfg.ap_channel = ap_channel->valueint;
    if (mode && cJSON_IsNumber(mode)) wifi_cfg.mode = mode->valueint;

    nvs_save_wifi_settings(&wifi_cfg);
    cJSON_Delete(json);
    ESP_LOGI("WIFI", " Mode: %d", wifi_cfg.mode);
    ESP_LOGI("WIFI", " STA SSID: %s", wifi_cfg.sta_ssid);
    ESP_LOGI("WIFI", " STA PASS: %s", wifi_cfg.sta_password);
    ESP_LOGI("WIFI", " AP SSID: %s", wifi_cfg.ap_ssid);
    ESP_LOGI("WIFI", " AP PASS: %s", wifi_cfg.ap_password);
    ESP_LOGI("WIFI", " AP CHANNEL: %d", wifi_cfg.ap_channel);
    httpd_resp_sendstr(req, "WiFi settings saved successfully 💾");
   
    return ESP_OK;      
  }


    if (section == SECTION_ACCOUNT) {

            char body[256] = {0};
            int ret = httpd_req_recv(req, body, sizeof(body) - 1);
            if (ret <= 0) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
                return ESP_FAIL;
            }
            body[ret] = '\0';

            ESP_LOGI("ACCOUNT", "Received body: %s", body);

            // -------- ПАРСИНГ form-urlencoded --------
            char raw_login[128] = {0};
            char raw_password[128] = {0};

            {
                char *p = strstr(body, "account-login=");
                if (p) {
                    p += strlen("account-login=");
                    sscanf(p, "%127[^&]", raw_login);
                }

                p = strstr(body, "account-password=");
                if (p) {
                    p += strlen("account-password=");
                    sscanf(p, "%127[^&]", raw_password);
                }
            }

            ESP_LOGI("ACCOUNT", "Raw parsed: login=%s password=%s", raw_login, raw_password);

            // -------- URL DECODE --------
            char login_dec[128] = {0};
            char pass_dec[128]  = {0};

            url_decode(login_dec, raw_login);
            url_decode(pass_dec, raw_password);

            ESP_LOGI("ACCOUNT", "Decoded: login=%s password=%s", login_dec, pass_dec);

            // -------- ЗАГРУЖАЕМ СТАРЫЕ НАСТРОЙКИ --------
            user_settings_t user_cfg = {0};
            nvs_load_user_settings(&user_cfg);

            // -------- ОБНОВЛЯЕМ --------
            if (strlen(login_dec) > 0)
                strncpy(user_cfg.account_login, login_dec, sizeof(user_cfg.account_login) - 1);

            if (strlen(pass_dec) > 0)
                strncpy(user_cfg.account_password, pass_dec, sizeof(user_cfg.account_password) - 1);

            // -------- СОХРАНЯЕМ --------
            esp_err_t err = nvs_save_user_settings(&user_cfg);
            if (err != ESP_OK) {
                ESP_LOGE("ACCOUNT", "Failed to save: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
                return ESP_FAIL;
            }

            ESP_LOGI("ACCOUNT", "Saved OK: login=%s password=%s",user_cfg.account_login, user_cfg.account_password);
            memcpy(&user, &user_cfg, sizeof(user_settings_t));
            websocket_restart(user_cfg.account_login, user_cfg.account_password);
            httpd_resp_sendstr(req, "Account settings saved successfully 💾");
            return ESP_OK;
        }

  if (section == SECTION_USER) {

 char buf[256];
    int len = req->content_len;
    if (len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    buf[len] = '\0'; // terminate string

    // --- Простая form-urlencoded разборка ---
    char login_val[64] = {0};
    char password_val[64] = {0};

    sscanf(strstr(buf, "login=") ?: "", "login=%63[^&]", login_val);
    sscanf(strstr(buf, "password=") ?: "", "password=%63[^&]", password_val);
    esp_err_t err = ESP_OK;
     // --- Сохраняем User Settings ---
    if (strlen(login_val) > 0 || strlen(password_val) > 0) {
        user_settings_t user = {0};
        nvs_load_user_settings(&user); // читаем старые данные
        if (strlen(login_val) > 0) strncpy(user.login, login_val, sizeof(user.login) - 1);
        if (strlen(password_val) > 0) strncpy(user.password, password_val, sizeof(user.password) - 1);
        err = nvs_save_user_settings(&user);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save user settings: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save user settings");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "User settings saved: %s / %s", user.login, user.password);
    }
       httpd_resp_sendstr(req, "User settings saved successfully 💾");
       return ESP_OK;
  }

    char buf[256];
    int len = req->content_len;
    if (len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    buf[len] = '\0'; // terminate string

    // --- Простая form-urlencoded разборка ---
   // char login_val[64] = {0};
  //  char password_val[64] = {0};
    char ssid_val[64] = {0};
    char mode_val[16] = {0};

  //  sscanf(strstr(buf, "login=") ?: "", "login=%63[^&]", login_val);
   // sscanf(strstr(buf, "password=") ?: "", "password=%63[^&]", password_val);
    sscanf(strstr(buf, "ssid=") ?: "", "ssid=%63[^&]", ssid_val);
    sscanf(strstr(buf, "mode=") ?: "", "mode=%15[^&]", mode_val);

    esp_err_t err = ESP_OK;

    

    // --- Сохраняем Network Settings ---
    if (strlen(ssid_val) > 0 || strlen(mode_val) > 0) {
        network_settings_t net = {0};
        nvs_load_network_settings(&net);
        if (strlen(ssid_val) > 0) strncpy(net.ap_ssid, ssid_val, sizeof(net.ap_ssid) - 1);
        if (strlen(mode_val) > 0) {
            if (strcmp(mode_val, "WIFI_MODE_NULL") == 0) net.mode = WIFI_MODE_NULL;
            else if (strcmp(mode_val, "WIFI_MODE_STA") == 0) net.mode = WIFI_MODE_STA;
            else if (strcmp(mode_val, "WIFI_MODE_AP") == 0) net.mode = WIFI_MODE_AP;
            else if (strcmp(mode_val, "WIFI_MODE_APSTA") == 0) net.mode = WIFI_MODE_APSTA;
        }
        err = nvs_save_network_settings(&net);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save network settings: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save network settings");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Network settings saved: ssid=%s mode=%s", net.ap_ssid, wifi_mode_to_string(net.mode));
    }

    httpd_resp_sendstr(req, "Settings saved successfully 💾");
    return ESP_OK;
}




/// POST /logout

static esp_err_t logout_post_handler(httpd_req_t *req)
{
    if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Logout called with header: %s", auth_token);
   
    // ===== 1. Обнуляем текущий токен =====
    memset(auth_token, 0, sizeof(auth_token));
    ESP_LOGI(TAG, "User token cleared (logout)");

    // ===== 2. Закрываем все WS-соединения =====
    /*
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] > 0) {
            ESP_LOGI(TAG, "Closing WS client: sock=%d", ws_clients[i]);
            httpd_ws_client_disconnect(server, ws_clients[i]);
            ws_clients[i] = 0;
        }
    }   */

    // ===== 3. Отправляем ответ =====
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    return ESP_OK;
}


// === POST /reboot ===
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Reboot requested from Web UI");

    // Ответ для фронта
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    cJSON_AddStringToObject(json, "message", "Rebooting");
    const char *resp = cJSON_PrintUnformatted(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    free((void*)resp);
    cJSON_Delete(json);

    // Небольшая задержка, чтобы ответ успел уйти
    vTaskDelay(300 / portTICK_PERIOD_MS);

    ESP_LOGW(TAG, "Performing restart...");
    esp_restart();

    return ESP_OK;
}

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    return httpd_resp_send(req, NULL, 0);
}

// ==== Конфигурация сервера ====
esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.uri_match_fn = httpd_uri_match_wildcard;
    config.ctrl_port = 32768;
    config.server_port = net.port;
    config.max_uri_handlers = 20;  
    config.max_open_sockets = 10;
    config.stack_size = 8192;
    config.enable_so_linger = false;
    config.linger_timeout = 0;
    config.open_fn  = NULL;
    config.close_fn = NULL;
   

    if (httpd_start(&server, &config) == ESP_OK) {

        // ====== 0. OPTIONS for root ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });

        // ====== 0.1 OPTIONS wildcard ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/*",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });

            // ====== 1. OPTIONS (точечные) ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/get_settings",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/get_settings/*",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/save_settings",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/save_settings/*",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });

        // ====== 2. API эндпоинты ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/login",
            .method = HTTP_POST,
            .handler = login_post_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/logout",
            .method = HTTP_POST,
            .handler = logout_post_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/reboot",
            .method = HTTP_POST,
            .handler = reboot_post_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/get_settings",
            .method = HTTP_GET,
            .handler = get_settings_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/get_settings/*",
            .method = HTTP_GET,
            .handler = get_settings_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/save_settings",
            .method = HTTP_POST,
            .handler = save_settings_post_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/save_settings/*",
            .method = HTTP_POST,
            .handler = save_settings_post_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true
        });

        // ====== 3. СТАТИЧНЫЕ ФАЙЛЫ — ВСЕГДА ПОСЛЕДНИЕ ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/*",
            .method = HTTP_GET,
            .handler = file_get_handler
        });

         ESP_LOGI(TAG, "Web server started on port %d", config.server_port);

        return ESP_OK;

    }

    ESP_LOGE(TAG, "Failed to start web server");
    return ESP_FAIL;
}

esp_err_t web_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
    return ESP_OK;
}




// URL-decode: decodes %XX and + → space
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if (*src == '%' &&
            isxdigit((unsigned char)src[1]) &&
            isxdigit((unsigned char)src[2])) {

            a = src[1];
            b = src[2];

            a = (a <= '9' ? a - '0' : toupper(a) - 'A' + 10);
            b = (b <= '9' ? b - '0' : toupper(b) - 'A' + 10);

            *dst++ = (char)(a * 16 + b);
            src += 3;
        }
        else if (*src == '+') {
            *dst++ = ' ';
            src++;
        }
        else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}


