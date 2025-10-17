


#include "web_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"  
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "nvs_credentials.h"
#include "esp_random.h"
#include "cJSON.h" 
#include <string.h>


static const char *TAG = "web_server";
static httpd_handle_t server = NULL;
static char auth_token[64] = {0};

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

    char input_login[64] = {0};
    char input_pass[64] = {0};

    sscanf(buf, "login=%63[^&]&password=%63s", input_login, input_pass);

    char stored_login[64] = {0};
    char stored_pass[64] = {0};
    if (nvs_load_credentials(stored_login, sizeof(stored_login), stored_pass, sizeof(stored_pass)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No credentials saved");
        return ESP_FAIL;
    }

    if (strcmp(input_login, stored_login) == 0 && strcmp(input_pass, stored_pass) == 0) {
        generate_token(auth_token, sizeof(auth_token));
        ESP_LOGI(TAG, "User '%s' authenticated, token: %s", input_login, auth_token);

        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "token", auth_token);
        const char *response = cJSON_PrintUnformatted(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
        cJSON_Delete(json);
        free((void*)response);
    } else {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid credentials 💩");
    }

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



// POST /save_settings
static esp_err_t save_settings_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, len = req->content_len;
    if (len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
        return ESP_FAIL;
    }
    buf[len] = '\0'; // terminate string

    // Простейший парсинг form-urlencoded: login=xxx&password=yyy&ssid=zzz&mode=STA
    char *login = strstr(buf, "login=");
    char *password = strstr(buf, "password=");
    char *ssid = strstr(buf, "ssid=");
    char *mode = strstr(buf, "mode=");
    
    if (!login || !password) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing login or password");
        return ESP_FAIL;
    }

    // выделяем значения
    char login_val[64] = {0};
    char password_val[64] = {0};
    char ssid_val[64] = {0};
    char mode_val[16] = {0};

    sscanf(login, "login=%63[^&]", login_val);
    sscanf(password, "password=%63[^&]", password_val);
    if (ssid) sscanf(ssid, "ssid=%63[^&]", ssid_val);
    if (mode) sscanf(mode, "mode=%15[^&]", mode_val);

    // Сохраняем логин/пароль через NVS
    esp_err_t err = nvs_save_credentials(login_val, password_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return ESP_FAIL;
    }

    // TODO: тут можно сохранять ssid_val и mode_val в отдельный NVS модуль, например nvs_network

    ESP_LOGI(TAG, "Saved credentials: %s / %s", login_val, password_val);
    httpd_resp_sendstr(req, "Settings saved successfully 💾");
    return ESP_OK;
}

// ==== Конфигурация сервера ====
esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.server_port = 80;

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


