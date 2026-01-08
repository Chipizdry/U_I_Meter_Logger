




#include "server_handlers.h"
#include "gpio_manager.h"
#include "web_server.h"
#include "esp_littlefs.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include "esp_http_server.h"
static const char *TAG = "server_handlers";




esp_err_t file_get_handler(httpd_req_t *req)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    // === CAPTIVE PORTAL ONLY FOR AP ===
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {

        // Не трогаем API, WS и статику
        if (!check_token(req) &&
            strcmp(req->uri, "/") != 0 &&
            !strstr(req->uri, ".js") &&
            !strstr(req->uri, ".css") &&
            !strstr(req->uri, ".png") &&
            !strstr(req->uri, ".jpg")) {

            ESP_LOGW("CAPTIVE", "Redirecting captive request: %s", req->uri);

            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }

    // ===== Обычная раздача файлов =====
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


static bool is_static_resource(const char *uri)
{
    if (!uri) return false;

    const char *exts[] = { ".css", ".js", ".png", ".jpg", ".ico", ".svg", ".woff", ".ttf" };
    for (size_t i = 0; i < sizeof(exts)/sizeof(exts[0]); i++) {
        if (strstr(uri, exts[i])) {
            return true;
        }
    }
    return false;
}


esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    return httpd_resp_send(req, NULL, 0); // пустой ответ OK
}


esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    // Работает только в AP или AP+STA
    if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        return ESP_ERR_NOT_FOUND;
    }

    // Уже авторизован — не трогаем
    if (check_token(req)) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *uri = req->uri;

    // Пропускаем статику, API и WebSocket
    if ((strncmp(uri, "/api", 4) == 0 ||
     strncmp(uri, "/ws", 3) == 0 ||
     is_static_resource(uri)) &&
    strcmp(uri, "/hotspot-detect.html") != 0 &&
    strcmp(uri, "/generate_204") != 0) {
    return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGW("CAPTIVE", "Captive hit: %s", uri);

    // --- Windows probes ---
    if (strcmp(uri, "/connecttest.txt") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strcmp(uri, "/ncsi.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Microsoft NCSI");
        return ESP_OK;
    }

    // --- iOS / Android probes ---
    if (strcmp(uri, "/hotspot-detect.html") == 0 ||
        strcmp(uri, "/generate_204") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Expires", "0");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<html><body>"
            "Redirecting to <a href='http://192.168.4.1/'>captive portal</a>"
            "</body></html>");
        return ESP_OK;
    }

    // --- Остальные URI → редирект на главную страницу AP ---
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body>"
        "Redirecting to <a href='http://192.168.4.1/'>captive portal</a>"
        "</body></html>");
    return ESP_OK;
}



esp_err_t ping_handler(httpd_req_t *req)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", sys.build_number);  // преобразуем число в строку
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}




/// POST /logout

esp_err_t logout_post_handler(httpd_req_t *req)
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



esp_err_t factory_reset_post_handler(httpd_req_t *req)
{

     if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "WEB: factory reset endpoint called");

    // Ответ для фронта
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req,"{\"status\":\"ok\",\"message\":\"Factory reset started\"}");
    ESP_LOGW(TAG, "Performing reset...");
    reset_to_factory_defaults();
    return ESP_OK;
}




// === POST /reboot ===
esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!check_token(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Reboot requested from Web UI");

    // Ответ для фронта
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); 
    httpd_resp_sendstr(req,"{\"status\":\"ok\",\"message\":\"Rebooting...\"}");

    // Небольшая задержка, чтобы ответ успел уйти
    vTaskDelay(500 / portTICK_PERIOD_MS);

    ESP_LOGW(TAG, "Performing restart...");
    esp_restart();

    return ESP_OK;
}
