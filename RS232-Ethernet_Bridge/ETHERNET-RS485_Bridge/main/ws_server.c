
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

#define MAX_CLIENTS 8

typedef struct {
    httpd_req_t *req;
    bool authorized;
} ws_client_t;

static ws_client_t ws_clients[MAX_CLIENTS] = {{0}};
void cancel_test_account(void);

/* ---------------- CLIENT MANAGEMENT ---------------- */
static void add_client(httpd_req_t *req) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i].req == req) return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!ws_clients[i].req) {
            ws_clients[i].req = req;
            ws_clients[i].authorized = false;
            network_notify_ws();
            return;
        }
}

static void remove_client(httpd_req_t *req) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i].req == req) {
            ws_clients[i].req = NULL;
            ws_clients[i].authorized = false;
            return;
        }
}

static ws_client_t* get_client(httpd_req_t *req) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i].req == req) return &ws_clients[i];
    return NULL;
}

/* ---------------- WS SEND / BROADCAST ---------------- */
void ws_send_fd(httpd_req_t *req, const char *msg) {
    if (!req || !msg) return;

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return;

    httpd_ws_frame_t ws_frame;
    ws_frame.type = HTTPD_WS_TYPE_TEXT;
    ws_frame.payload = (uint8_t*)msg;
    ws_frame.len = strlen(msg);
    ws_frame.final = true;

    esp_err_t ret = httpd_ws_send_frame_async(server, fd, &ws_frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS send failed: %d", ret);
    }
}

void ws_broadcast(const char* text) {
    if (!server || !text) return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (ws_clients[i].req)
            ws_send_fd(ws_clients[i].req, text);
}

/* ---------------- CUSTOM ACTION HANDLER ---------------- */
void handle_ws_custom_message(httpd_req_t *req, cJSON *msg) {
    if (!msg) return;

    ws_client_t *client_obj = get_client(req);
    if (!client_obj) return;

    cJSON *action = cJSON_GetObjectItem(msg, "action");
    if (!action || !cJSON_IsString(action)) return;

    const char *act = action->valuestring;

    if (strcmp(act, "test_account") == 0) {
        cJSON *login = cJSON_GetObjectItem(msg, "account_login");
        cJSON *password = cJSON_GetObjectItem(msg, "account_password");
        cJSON *node_name = cJSON_GetObjectItem(msg, "node_name");

        if (!login || !password || !cJSON_IsString(login) || !cJSON_IsString(password)) {
            ws_send_fd(req, "{\"error\":\"invalid test_account payload\"}");
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
        ws_send_fd(req, "{\"cloud_status\":\"test_account ok\"}");
        return;
    }

    if (strcmp(act, "cancel_test_account") == 0) {
        cancel_test_account();
        ws_send_fd(req, "{\"cloud_status\":\"test_account cancelled\"}");
        return;
    }

    if (strcmp(act, "wifi_scan") == 0) {
        wifi_scan_networks();
        ws_send_fd(req, "{\"type\":\"wifi_scan\",\"status\":\"started\"}");
        return;
    }

    if (strcmp(act, "diagnostics_on") == 0) {
        diagnostics_active = true;
        ws_send_fd(req, "{\"diagnostics\":\"on\"}");
        return;
    }

    if (strcmp(act, "diagnostics_off") == 0) {
        diagnostics_active = false;
        ws_send_fd(req, "{\"diagnostics\":\"off\"}");
        return;
    }

    ws_send_fd(req, "{\"error\":\"unknown action\"}");
}

/* ---------------- MAIN WS HANDLER ---------------- */
esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        add_client(req);
        return ESP_OK;
    }

    char buf[1024];
    httpd_ws_frame_t ws_pkt;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)buf;
    ws_pkt.len = sizeof(buf);

   esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret <= 0) {
        remove_client(req);
        return ESP_FAIL;
    }

    buf[ws_pkt.len] = 0;

    // ping/pong
    if (strcmp(buf, "ping") == 0) {
        ws_send_fd(req, "pong");
        return ESP_OK;
    }
    if (strcmp(buf, "pong") == 0) return ESP_OK;

    cJSON *msg = cJSON_Parse(buf);
    if (!msg) return ESP_OK;

    // Авторизация
    cJSON *type = cJSON_GetObjectItem(msg, "type");
    if (type && strcmp(type->valuestring, "auth") == 0) {
        cJSON *token = cJSON_GetObjectItem(msg, "token");
        if (token && strcmp(token->valuestring, auth_token) == 0) {
            ws_client_t* client_obj = get_client(req);
            if (client_obj) client_obj->authorized = true;
            network_notify_ws();
        } else {
            remove_client(req);
            ws_send_fd(req, "");
        }
        cJSON_Delete(msg);
        return ESP_OK;
    }

    ws_client_t* client_obj = get_client(req);
    if (!client_obj || !client_obj->authorized) {
        cJSON_Delete(msg);
        return ESP_OK;
    }

    handle_ws_custom_message(req, msg);
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

