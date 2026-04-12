

#include "ws_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "websocket_client.h"
#include "wifi_manager.h"
#include "esp_websocket_client.h"
#include "rs485_master.h"
#include "nvs_settings.h"
#include "network_state.h"

#define WS_LOG(fmt, ...) ESP_LOGI(TAG, "[WS] " fmt, ##__VA_ARGS__)

// --- Внешние переменные ---
extern char auth_token[64];
extern httpd_handle_t server;
extern char ws_email[64];
extern char ws_password[64];
extern char ws_node_name[32];
extern esp_websocket_client_handle_t client;
extern uint32_t last_ws_event_tick;
extern bool ws_connected;
extern user_settings_t user_cfg;

static const char *TAG = "ws_server";
bool test_account_active = false;
bool diagnostics_active = false;

#define MAX_CLIENTS 4

typedef struct {
    int fd;
    bool authorized;
} ws_client_t;

static ws_client_t ws_clients[MAX_CLIENTS] = {{0}};
void cancel_test_account(void);
void ws_cleanup_all_clients(void);
void ws_notify_reconnect(void);
/* ---------------- CLIENT MANAGEMENT ---------------- */
static ws_client_t* find_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i].fd == fd)
            return &ws_clients[i];
    return NULL;
}

static void add_client(int fd) {
    ws_client_t *existing = find_client(fd);
    if (existing) {
        WS_LOG("FD=%d already exists → reusing slot", fd);
        return; // старый слот используем, не удаляем
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd == 0) {
            ws_clients[i].fd = fd;
            ws_clients[i].authorized = false;
            WS_LOG("New client slot assigned: FD=%d", fd);
            return;
        }
    }

    WS_LOG("No free client slots for FD=%d", fd);
}

static void remove_client(int fd) {
    ws_client_t *client = find_client(fd);
    if (client) {
        WS_LOG("Removing client FD=%d", fd);
        client->fd = 0;
        client->authorized = false;
    }
}


void ws_cleanup_all_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd != 0) {
            // Проверяем статус сокета
            if (server && httpd_ws_get_fd_info(server, ws_clients[i].fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
                ESP_LOGW(TAG, "Cleaning up stale client FD=%d", ws_clients[i].fd);
                ws_clients[i].fd = 0;
                ws_clients[i].authorized = false;
            }
        }
    }
}


/* ---------------- WS SEND / BROADCAST ---------------- */


void ws_send_fd(int fd, const char *msg) {
    if (!server || fd <= 0) return;

    ws_client_t *client = find_client(fd);
    if (!client) {
        WS_LOG("WS send aborted: FD=%d not valid", fd);
        return;
    }

    // 🔴 Проверка состояния сокета
    if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        WS_LOG("FD=%d is not active WS → removing", fd);
        remove_client(fd);
        return;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)msg,
        .len = strlen(msg),
        .final = true
    };

    esp_err_t ret = httpd_ws_send_frame_async(server, fd, &frame);

    if (ret != ESP_OK) {
        WS_LOG("WS send failed → removing client FD=%d (%s)",
               fd, esp_err_to_name(ret));

        // 🔥 ВАЖНО: удаляем клиента
        remove_client(fd);
    } else {
        WS_LOG("WS sent OK, FD=%d", fd);
    }
}



void ws_broadcast(const char* text) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i].fd)
            ws_send_fd(ws_clients[i].fd, text);
}

/* ---------------- CUSTOM ACTION HANDLER ---------------- */
void handle_ws_custom_message(int fd, cJSON *msg) {
    ws_client_t *client_obj = find_client(fd);
    if (!client_obj || !msg) return;

    cJSON *action = cJSON_GetObjectItem(msg, "action");
    if (!action || !cJSON_IsString(action)) return;

    const char *act = action->valuestring;

    if (strcmp(act, "test_account") == 0) {
        cJSON *login = cJSON_GetObjectItem(msg, "account_login");
        cJSON *password = cJSON_GetObjectItem(msg, "account_password");
        cJSON *node_name = cJSON_GetObjectItem(msg, "node_name");

        if (!login || !password || !cJSON_IsString(login) || !cJSON_IsString(password)) {
            ws_send_fd(fd, "{\"error\":\"invalid test_account payload\"}");
            return;
        }

        strncpy(ws_email, login->valuestring, sizeof(ws_email)-1);
        strncpy(ws_password, password->valuestring, sizeof(ws_password)-1);
        strncpy(ws_node_name, node_name->valuestring, sizeof(ws_node_name)-1);
        test_account_active = true;

        if (client) {
            esp_websocket_client_stop(client);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_websocket_client_destroy(client);
            client = NULL;
        }

        ws_connected = false;
        last_ws_event_tick = 0;

        websocket_client_start(user_cfg.serial, ws_email, ws_password, ws_node_name);
        ws_send_fd(fd, "{\"cloud_status\":\"test_account ok\"}");
        return;
    }

    if (strcmp(act, "cancel_test_account") == 0) {
        cancel_test_account();
        ws_send_fd(fd, "{\"cloud_status\":\"test_account cancelled\"}");
        return;
    }

    if (strcmp(act, "wifi_scan") == 0) {
        wifi_scan_networks();
        return;
    }

    if (strcmp(act, "diagnostics_on") == 0) {
        diagnostics_active = true;
        ws_send_fd(fd, "{\"diagnostics\":\"on\"}");
        return;
    }

    if (strcmp(act, "diagnostics_off") == 0) {
        diagnostics_active = false;
        ws_send_fd(fd, "{\"diagnostics\":\"off\"}");
        return;
    }

    ws_send_fd(fd, "{\"error\":\"unknown action\"}");
}

/* ---------------- MAIN WS HANDLER ---------------- */
esp_err_t ws_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    WS_LOG("WS handler invoked. FD=%d, method=%s", fd,
           req->method == HTTP_GET ? "GET" : "OTHER");

    if (req->method == HTTP_GET) {
        add_client(fd);
        WS_LOG("Client added, FD=%d", fd);
        return ESP_OK;
    }

    char buf[1024];
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len == 0) return ESP_OK;
    if (ws_pkt.len > sizeof(buf) - 1) return ESP_FAIL;

    ws_pkt.payload = (uint8_t*)buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) return ret;

    buf[ws_pkt.len] = '\0';
    WS_LOG("Received WS payload: %s", buf);

    ws_client_t *client_obj = find_client(fd);
    if (!client_obj) add_client(fd);

    // Ping/Pong
    if (strcmp(buf, "ping") == 0) { ws_send_fd(fd, "pong"); return ESP_OK; }
    if (strcmp(buf, "pong") == 0) return ESP_OK;

    // JSON parse
    cJSON *msg = cJSON_Parse(buf);
    if (!msg) return ESP_OK;

    cJSON *type = cJSON_GetObjectItem(msg, "type");
    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "auth") == 0) {
        cJSON *token = cJSON_GetObjectItem(msg, "token");
        if (token && strcmp(token->valuestring, auth_token) == 0) {
            client_obj->authorized = true;
            ws_send_fd(fd, "{\"auth_status\":\"ok\"}");
            network_notify_ws();
            vTaskDelay(50);
            extern void send_pending_scan_results(void);
            send_pending_scan_results();
        } else {
            ws_send_fd(fd, "{\"auth_status\":\"fail\"}");
            remove_client(fd);
        }
        cJSON_Delete(msg);
        return ESP_OK;
    }

    if (!client_obj->authorized) {
        cJSON_Delete(msg);
        return ESP_OK;
    }

    handle_ws_custom_message(fd, msg);
    cJSON_Delete(msg);
    return ESP_OK;
}

/* ---------------- CANCEL TEST ACCOUNT ---------------- */
void cancel_test_account(void) {
    test_account_active = false;
    strncpy(ws_email, user_cfg.account_login, sizeof(ws_email)-1);
    strncpy(ws_password, user_cfg.account_password, sizeof(ws_password)-1);

    websocket_disable_reconnect();
    if (client) {
        esp_websocket_client_stop(client);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_destroy(client);
        client = NULL;
    }

    websocket_client_start(user_cfg.serial, ws_email, ws_password, ws_node_name);
    websocket_enable_reconnect();
}


void ws_notify_reconnect(void) {
    // Отправляем всем клиентам команду на перезагрузку страницы
    const char *reload_msg = "{\"type\":\"reload\",\"reason\":\"connection_lost\"}";
    ws_broadcast(reload_msg);
    ESP_LOGI(TAG, "Sent reload notification to all clients");
}


