


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
#include <stdbool.h>
#include "ota_update.h"


static const char *TAG = "web_server";
static httpd_handle_t server = NULL;
 char auth_token[64] = {0};




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



// === GET /get_settings ===
static esp_err_t get_settings_handler(httpd_req_t *req)
{
    if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

     // Определяем подмаршрут
    const char *uri = req->uri;

     
    bool want_user = (strstr(uri, "/uart") != NULL);
    bool want_network = (strstr(uri, "/network") != NULL);
    bool want_system = (strstr(uri, "/system") != NULL);
    bool want_all = (!want_user && !want_network && !want_system);



    // --- Загружаем все настройки из NVS ---
    user_settings_t user = {0};
    network_settings_t net = {0};
    system_settings_t sys = {0};
    uart_settings_t uart_cfg = {0};

    nvs_load_user_settings(&user);
    nvs_load_network_settings(&net);
    nvs_load_system_settings(&sys);
    nvs_load_uart_settings(&uart_cfg);
    

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
    }



    // --- Формируем JSON ---
    cJSON *json = cJSON_CreateObject();

    cJSON *user_json = cJSON_CreateObject();
    cJSON_AddStringToObject(user_json, "login", user.login);
    cJSON_AddStringToObject(user_json, "language", user.language);
    cJSON_AddItemToObject(json, "user", user_json);

    cJSON *network_json = cJSON_CreateObject();
    cJSON_AddStringToObject(network_json, "ip", net.ip);
    cJSON_AddStringToObject(network_json, "gateway", net.gateway);
    cJSON_AddStringToObject(network_json, "mask", net.mask);
    cJSON_AddStringToObject(network_json, "dns", net.dns);
    cJSON_AddNumberToObject(network_json, "port", net.port);
    cJSON_AddBoolToObject(network_json, "dhcp_enabled", net.dhcp_enabled);
    cJSON_AddStringToObject(network_json, "ssid", net.ap_ssid);
    cJSON_AddStringToObject(network_json, "mode", wifi_mode_to_string(net.mode));
    cJSON_AddItemToObject(json, "network", network_json);

    cJSON *system_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(system_json, "refresh_interval", sys.refresh_interval);
    cJSON_AddNumberToObject(system_json, "log_level", sys.log_level);
    cJSON_AddBoolToObject(system_json, "debug_mode", sys.debug_mode);
    cJSON_AddItemToObject(json, "system", system_json);

    const char *response = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);

    cJSON_Delete(json);
    free((void*)response);

    return ESP_OK;
}






// ==== Отправка статического файла из /littlefs ====
static esp_err_t file_get_handler(httpd_req_t *req)
{
    char filepath[128] = "/littlefs";
    const char *uri = req->uri;  // <-- напрямую берём URI из структуры

    // "/" → "/index.html"
    if (strcmp(uri, "/") == 0) {
        strcat(filepath, "/main.html");
    } else {
        strcat(filepath, uri);
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
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
    char ssid_val[64] = {0};
    char mode_val[16] = {0};

    sscanf(strstr(buf, "login=") ?: "", "login=%63[^&]", login_val);
    sscanf(strstr(buf, "password=") ?: "", "password=%63[^&]", password_val);
    sscanf(strstr(buf, "ssid=") ?: "", "ssid=%63[^&]", ssid_val);
    sscanf(strstr(buf, "mode=") ?: "", "mode=%15[^&]", mode_val);

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




// ==== Конфигурация сервера ====
esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.server_port = net.port;
    config.max_uri_handlers = 15;  
    config.max_open_sockets = 6;
    config.stack_size = 8192;
   

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t file_get_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = file_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t save_settings_uri = {
            .uri = "/save_settings",
            .method = HTTP_POST,
            .handler = save_settings_post_handler,
            .user_ctx = NULL
        };
        httpd_uri_t login_uri = {
            .uri = "/login",
            .method = HTTP_POST,
            .handler = login_post_handler,
            .user_ctx = NULL
        };
        httpd_uri_t get_settings_uri = {
            .uri = "/get_settings",
            .method = HTTP_GET,
            .handler = get_settings_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/get_settings/*",
            .method = HTTP_GET,
            .handler = get_settings_handler,
            .user_ctx = NULL
        });

        httpd_uri_t ota_uri = {
            .uri = "/ota",
            .method = HTTP_POST,
            .handler = ota_post_handler,
            .user_ctx = NULL
        };
        
        httpd_register_uri_handler(server, &ota_uri);
        httpd_register_uri_handler(server, &get_settings_uri);
        httpd_register_uri_handler(server, &file_get_uri);
        httpd_register_uri_handler(server, &save_settings_uri);
        httpd_register_uri_handler(server, &login_uri);
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


