


#include "websocket_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_server";
static httpd_handle_t ws_server = NULL;

#define MAX_CLIENTS 8
static int ws_clients[MAX_CLIENTS] = {0};

/* ------------------------ CLIENT LIST ------------------------ */

static void add_client(int sockfd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] == 0) {
            ws_clients[i] = sockfd;
            ESP_LOGI(TAG, "Client connected: %d", sockfd);
            return;
        }
    }
    ESP_LOGW(TAG, "Client list full!");
}

static void remove_client(int sockfd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] == sockfd) {
            ws_clients[i] = 0;
            ESP_LOGI(TAG, "Client disconnected: %d", sockfd);
            return;
        }
    }
}

int websocket_server_get_client_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i] > 0) n++;
    return n;
}

/* ------------------------ WEBSOCKET HANDLER ------------------------ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    /* Устанавливаем клиента при рукопожатии */
    if (req->method == HTTP_GET) {
        int sockfd = httpd_req_to_sockfd(req);
        add_client(sockfd);
        ESP_LOGI(TAG, "WS handshake completed");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    /* 1 — Узнать длину сообщения */
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    uint8_t *buf = calloc(frame.len + 1, 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;

    /* 2 — Прочитать содержимое */
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    ESP_LOGI(TAG, "WS received: %s", buf);

    /* 3 — Ответ */
    httpd_ws_frame_t tx = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = buf,
        .len = frame.len
    };

    httpd_ws_send_frame(req, &tx);

    free(buf);
    return ESP_OK;
}

/* ------------------------ CLIENT CLOSE CALLBACK ------------------------ */

static void on_client_disconnected(httpd_handle_t hd, int sockfd) {
    remove_client(sockfd);
}

/* ------------------------ SERVER START ------------------------ */

esp_err_t websocket_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 8;
    config.close_fn = on_client_disconnected;

    if (httpd_start(&ws_server, &config) != ESP_OK)
        return ESP_FAIL;

    httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL
    };

    httpd_register_uri_handler(ws_server, &ws_uri);
    ESP_LOGI(TAG, "WebSocket server started at /ws");

    return ESP_OK;
}

/* ------------------------ SEND ONE CLIENT ------------------------ */

esp_err_t websocket_server_send(httpd_req_t *req, const char *msg)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)msg,
        .len = strlen(msg)
    };

    return httpd_ws_send_frame(req, &frame);
}

/* ------------------------ BROADCAST ------------------------ */

esp_err_t websocket_server_broadcast(const char *msg)
{
    if (!ws_server) return ESP_FAIL;

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)msg,
        .len = strlen(msg)
    };

    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = ws_clients[i];
        if (fd > 0) {
            ESP_LOGI(TAG, "WS → %d: %s", fd, msg);
            httpd_ws_send_frame_async(ws_server, fd, &frame);
        }
    }
    return ESP_OK;
}





