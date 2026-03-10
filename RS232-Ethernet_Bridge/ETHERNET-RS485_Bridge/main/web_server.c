


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
#include "server_handlers.h"
#include "lwip/inet.h" 
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include <stdbool.h>
#include "ota_update.h"
#include "websocket_client.h"
#include "wifi_manager.h"
#include "rs485_master.h"
#include "web_auth.h"
#include "ws_server.h"
#include "gpio_manager.h"
#include "network_state.h"
#include "driver/uart.h"


static const char *TAG = "web_server";

static httpd_uri_t ws_uri = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true
};


extern char cloud_status_msg[32] ;   // статус по умолчанию
httpd_handle_t server = NULL;
static void url_decode(char *dst, const char *src);
void fill_netif_ip_info(const char *ifkey,char *ip,char *mask,char *gw,char *dns,size_t len);

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
    extern user_settings_t user_cfg;
    extern network_settings_t net_cfg;
    extern wifi_settings_t wifi_cfg;
    extern system_settings_t sys;
    extern uart_settings_t uart_cfg;

    // Если DHCP включен, берем текущие настройки с Ethernet
  
        if (net_cfg.dhcp_enabled) {
        fill_netif_ip_info("ETH_DEF", net_cfg.ip, net_cfg.mask,net_cfg.gateway,net_cfg.dns, sizeof(net_cfg.ip) );
    }


        if (net_cfg.wifi_dhcp_enabled &&
        (wifi_cfg.mode == WIFI_MODE_STA || wifi_cfg.mode == WIFI_MODE_APSTA)) {
        fill_netif_ip_info("WIFI_STA_DEF", net_cfg.wifi_ip, net_cfg.wifi_mask, net_cfg.wifi_gateway, net_cfg.wifi_dns,sizeof(net_cfg.wifi_ip) );
    }
    // Сбор JSON
    cJSON *json = cJSON_CreateObject();

    switch (section) {

        case ALL:
        case USER: {
            cJSON *u = cJSON_CreateObject();
            cJSON_AddStringToObject(u, "node_name", user_cfg.node_name); // ✅ имя узла
            cJSON_AddStringToObject(u, "login", user_cfg.login);
            cJSON_AddStringToObject(u, "account_login", user_cfg.account_login);  // ✅ имя аккаунта
            cJSON_AddStringToObject(u, "language", user_cfg.language);
            cJSON_AddStringToObject(u, "serial", user_cfg.serial);     // ✅ серийный номер      
            cJSON_AddBoolToObject(u, "connected", ws_connected); 
            cJSON_AddStringToObject(u, "status", cloud_status_msg);
            cJSON_AddItemToObject(json, "user", u);
            if (section != ALL) break;
            [[fallthrough]]; 
        }

        case NETWORK: {
            cJSON *n = cJSON_CreateObject();
            cJSON_AddStringToObject(n, "ip", net_cfg.ip);
            cJSON_AddStringToObject(n, "mask", net_cfg.mask);
            cJSON_AddStringToObject(n, "gateway", net_cfg.gateway);
            cJSON_AddStringToObject(n, "dns", net_cfg.dns); 
            cJSON_AddNumberToObject(n, "port", net_cfg.port);
            cJSON_AddBoolToObject(n, "dhcp_enabled", net_cfg.dhcp_enabled);

            cJSON_AddStringToObject(n, "wifi_ip", net_cfg.wifi_ip);
            cJSON_AddStringToObject(n, "wifi_mask", net_cfg.wifi_mask);
            cJSON_AddStringToObject(n, "wifi_gateway", net_cfg.wifi_gateway);
            cJSON_AddStringToObject(n, "wifi_dns", net_cfg.wifi_dns);
            cJSON_AddBoolToObject(n, "wifi_dhcp_enabled", net_cfg.wifi_dhcp_enabled);
            
            cJSON_AddItemToObject(json, "network", n);
            network_notify_ws();
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


    if (section == SECTION_NETWORK) {

    char body[512] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    body[ret] = '\0';

    ESP_LOGI("NETWORK", "Received NETWORK JSON: %s", body);

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }


    // ===== ETH =====
    cJSON *dhcp = cJSON_GetObjectItem(json, "dhcp_enabled");
    if (dhcp && cJSON_IsBool(dhcp)) {
        net_cfg.dhcp_enabled = cJSON_IsTrue(dhcp);
    }

    if (!net_cfg.dhcp_enabled) {
        cJSON *ip  = cJSON_GetObjectItem(json, "ip");
        cJSON *msk = cJSON_GetObjectItem(json, "mask");
        cJSON *gw  = cJSON_GetObjectItem(json, "gateway");
        cJSON *dns = cJSON_GetObjectItem(json, "dns");

        if (ip  && cJSON_IsString(ip))  strncpy(net_cfg.ip, ip->valuestring, sizeof(net_cfg.ip)-1);
        if (msk && cJSON_IsString(msk)) strncpy(net_cfg.mask, msk->valuestring, sizeof(net_cfg.mask)-1);
        if (gw  && cJSON_IsString(gw))  strncpy(net_cfg.gateway, gw->valuestring, sizeof(net_cfg.gateway)-1);
        if (dns && cJSON_IsString(dns)) strncpy(net_cfg.dns, dns->valuestring, sizeof(net_cfg.dns)-1);
    }

    cJSON *port = cJSON_GetObjectItem(json, "port");
    if (port && cJSON_IsNumber(port)) { net_cfg.port = port->valueint; }

    // ===== WIFI STA (IP only) =====
    cJSON *wifi_dhcp = cJSON_GetObjectItem(json, "wifi_dhcp_enabled");
    if (wifi_dhcp && cJSON_IsBool(wifi_dhcp)) {
        net_cfg.wifi_dhcp_enabled = cJSON_IsTrue(wifi_dhcp);
    }

    if (!net_cfg.wifi_dhcp_enabled) {
        cJSON *ip  = cJSON_GetObjectItem(json, "wifi_ip");
        cJSON *msk = cJSON_GetObjectItem(json, "wifi_mask");
        cJSON *gw  = cJSON_GetObjectItem(json, "wifi_gateway");
        cJSON *dns = cJSON_GetObjectItem(json, "wifi_dns");

        if (ip  && cJSON_IsString(ip))  strncpy(net_cfg.wifi_ip, ip->valuestring, sizeof(net_cfg.wifi_ip)-1);
        if (msk && cJSON_IsString(msk)) strncpy(net_cfg.wifi_mask, msk->valuestring, sizeof(net_cfg.wifi_mask)-1);
        if (gw  && cJSON_IsString(gw))  strncpy(net_cfg.wifi_gateway, gw->valuestring, sizeof(net_cfg.wifi_gateway)-1);
        if (dns && cJSON_IsString(dns)) strncpy(net_cfg.wifi_dns, dns->valuestring, sizeof(net_cfg.wifi_dns)-1);
    }

    // ===== SAVE =====
    nvs_save_network_settings(&net_cfg);

    ESP_LOGI("NETWORK", "ETH: DHCP=%d IP=%s" , net_cfg.dhcp_enabled, net_cfg.ip);
    ESP_LOGI("NETWORK", "WIFI: DHCP=%d IP=%s", net_cfg.wifi_dhcp_enabled, net_cfg.wifi_ip);

    cJSON_Delete(json);

    // ===== APPLY =====
  
    wifi_manager_request_apply();
    network_notify_ws();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req,"{\"status\":\"ok\",\"message\":\"Сетевые настройки сохранены !!!💾\"}");
    return ESP_OK;
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

        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Настройки UART сохранены !!!💾\"}");

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
    wifi_manager_request_apply();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Настройки Wi-Fi сохранены !!!💾\"}");
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

    // -------- PARSE form-urlencoded --------
    char raw_login[128] = {0};
    char raw_password[128] = {0};
    char raw_node_name[96] = {0};

    char *p;
    if ((p = strstr(body, "account-login="))) {
        p += strlen("account-login=");
        sscanf(p, "%127[^&]", raw_login);
    }

    if ((p = strstr(body, "account-password="))) {
        p += strlen("account-password=");
        sscanf(p, "%127[^&]", raw_password);
    }

    if ((p = strstr(body, "node-name="))) {
        p += strlen("node-name=");
        sscanf(p, "%95[^&]", raw_node_name);
    }

    // -------- URL decode --------
    char login_dec[128] = {0};
    char pass_dec[128]  = {0};
    char node_name_dec[32] = {0};

    url_decode(login_dec, raw_login);
    url_decode(pass_dec, raw_password);
    url_decode(node_name_dec, raw_node_name);

    // -------- UPDATE GLOBAL user_cfg --------
    if (strlen(login_dec) > 0)
        strncpy(user_cfg.account_login, login_dec, sizeof(user_cfg.account_login) - 1);

    if (strlen(pass_dec) > 0)
        strncpy(user_cfg.account_password, pass_dec, sizeof(user_cfg.account_password) - 1);

    if (strlen(node_name_dec) > 0)
        strncpy(user_cfg.node_name, node_name_dec, sizeof(user_cfg.node_name) - 1);

    // -------- SAVE TO NVS --------
    esp_err_t err = nvs_save_user_settings(&user_cfg);
    if (err != ESP_OK) {
        ESP_LOGE("ACCOUNT", "Failed to save: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    ESP_LOGI("ACCOUNT","Saved OK: login=%s password=%s node_name=%s",user_cfg.account_login,user_cfg.account_password,user_cfg.node_name);

    // -------- APPLY RUNTIME SIDE EFFECTS --------
    websocket_restart(user_cfg.account_login,user_cfg.account_password,user_cfg.node_name);

    // -------- RESPONSE --------
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req,
        "{\"status\":\"ok\",\"message\":\"Настройки аккаунта сохранены !!!💾\"}"
    );

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
        if (strlen(login_val) > 0) strncpy(user_cfg.login, login_val, sizeof(user_cfg.login) - 1);
        if (strlen(password_val) > 0) strncpy(user_cfg.password, password_val, sizeof(user_cfg.password) - 1);

        err = nvs_save_user_settings(&user_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save user settings: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save user settings");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "User settings saved: %s / %s", user_cfg.login, user_cfg.password);
    }
       // -------- HTTP RESPONSE --------
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req,"{\"status\":\"ok\",\"message\":\"Пользовательские настройки сохранены 💾\"}");
    return ESP_OK;
  }


    httpd_resp_sendstr(req, "Settings saved successfully 💾");
    return ESP_OK;
}


// ==== Конфигурация сервера ====
esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.ctrl_port = 32768;
    config.server_port = net_cfg.port;
    config.max_uri_handlers = 20;
    config.max_open_sockets = 10;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) == ESP_OK) {

          // ====== PING (должен быть ПЕРВЫМ) ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/ping",
            .method = HTTP_GET,
            .handler = ping_handler
        });

        // ====== ЕДИНСТВЕННЫЙ universal OPTIONS ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/*",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        });
     
         // ====== OTA UPDATE ======
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/ota",
            .method = HTTP_POST,
            .handler = ota_post_handler
        });

        
        // ====== API эндпоинты ======
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
            .uri = "/factory_reset",
            .method = HTTP_POST,
            .handler = factory_reset_post_handler
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

        httpd_register_uri_handler(server, &ws_uri);
      /*
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL
        });   */

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = captive_redirect_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/*",
            .method = HTTP_DELETE,
            .handler = captive_redirect_handler
        });

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/*",
            .method = HTTP_POST,
            .handler = captive_redirect_handler
        });
        // ====== СТАТИЧНЫЕ ФАЙЛЫ — всегда последними ======
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


 void fill_netif_ip_info(
    const char *ifkey,
    char *ip,
    char *mask,
    char *gw,
    char *dns,
    size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!netif) {
        ESP_LOGW("NET", "netif %s not found", ifkey);
        strcpy(ip, "0.0.0.0");
        strcpy(mask, "0.0.0.0");
        strcpy(gw, "0.0.0.0");
        strcpy(dns, "0.0.0.0");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        inet_ntoa_r(ip_info.ip, ip, len);
        inet_ntoa_r(ip_info.netmask, mask, len);
        inet_ntoa_r(ip_info.gw, gw, len);
    }

    esp_netif_dns_info_t dns_info;
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
        inet_ntoa_r(dns_info.ip.u_addr.ip4, dns, len);
    } else {
        strcpy(dns, "0.0.0.0");
    }
}


