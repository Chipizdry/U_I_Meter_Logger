


#include "web_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"  
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include <string.h>


static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

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
        httpd_register_uri_handler(server, &file_get_uri);
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


