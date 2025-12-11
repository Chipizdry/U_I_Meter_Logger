
#include "esp_log.h"
#include "cJSON.h" 
#include "nvs_settings.h"
//#include "web_server.h"
#include "web_auth.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "web_auth";

char auth_token[64] = {0};

void generate_token(char *buf, size_t len) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    buf[len - 1] = '\0';
}

bool check_token(httpd_req_t *req) {
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (len <= 1) return false;

    char *buf = malloc(len);
    httpd_req_get_hdr_value_str(req, "Authorization", buf, len);

    bool ok = false;
    if (strncmp(buf, "Bearer ", 7) == 0) {
        const char *t = buf + 7;
        if (strcmp(t, auth_token) == 0) ok = true;
    }

    free(buf);
    return ok;
}




// === POST /login ===
 esp_err_t login_post_handler(httpd_req_t *req)
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


