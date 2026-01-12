


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

// === Внешние переменные из web_server.c ===
extern char auth_token[64];
extern httpd_handle_t server;

// === Переменные облачного клиента ===
extern char ws_email[64];
extern char ws_password[64];
extern char ws_node_name[32];
extern esp_websocket_client_handle_t client;
extern uint32_t last_ws_event_tick;

extern bool ws_connected;
extern user_settings_t user;   // из get_settings()

static const char *TAG = "ws_server";
bool test_account_active = false;

bool diagnostics_active = false;

#define MAX_CLIENTS 8

typedef struct {
    int fd;
    bool authorized;
} ws_client_t;

static ws_client_t ws_clients[MAX_CLIENTS] = {{0, false}};
void cancel_test_account(void);

/* ---------------- CLIENT MANAGEMENT ---------------- */
static void add_client(int sockfd)
{
    // Проверяем, есть ли уже такой fd
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd == sockfd) {
          //  ESP_LOGI(TAG, "WS: client already added, fd=%d (slot %d)", sockfd, i);
            return;
        }
    }

    // Добавляем нового клиента
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd == 0) {
            ws_clients[i].fd = sockfd;
            ws_clients[i].authorized = false;
          //  ESP_LOGI(TAG, "WS: client added, fd=%d (slot %d)", sockfd, i);
            return;
        }
    }
   // ESP_LOGW(TAG, "WS: no free client slots!");
}

static void remove_client(int sockfd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd == sockfd) {
          //  ESP_LOGI(TAG, "WS: client removed, fd=%d (slot %d)", sockfd, i);
            ws_clients[i].fd = 0;
            ws_clients[i].authorized = false;
            return;
        }
    }
}

static ws_client_t* get_client(int sockfd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd == sockfd)
            return &ws_clients[i];
    }
    return NULL;
}



// -------------------------------------------------------------------
//  WS SEND / BROADCAST
// -------------------------------------------------------------------


 void ws_send(int client_fd, const char* text) {
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)text,
        .len = strlen(text)
    };
    httpd_ws_send_frame_async(server, client_fd, &frame);
}


void ws_broadcast(const char *text)
{
    if (!server) return;

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)text,
        .len = strlen(text)
    };

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd > 0) {
            httpd_ws_send_frame_async(server, ws_clients[i].fd, &frame);
        }
    }
}

// -------------------------------------------------------------------
//  CUSTOM ACTION HANDLER ("test_account")
// -------------------------------------------------------------------

void handle_ws_custom_message(int client_fd, cJSON *msg) {
    if (!msg) return;

    // Главное поле — "action"
    cJSON *action = cJSON_GetObjectItem(msg, "action");
    if (!action || !cJSON_IsString(action)) {
        ESP_LOGW("WS", "WS JSON without 'action' from fd=%d", client_fd);
        return;
    }

    const char *act = action->valuestring;
    ESP_LOGI("WS", "Handling action='%s' from fd=%d", act, client_fd);

    // === ACTION: test_account ===
    if (strcmp(act, "test_account") == 0) {

        cJSON *login = cJSON_GetObjectItem(msg, "account_login");
        cJSON *password = cJSON_GetObjectItem(msg, "account_password");
        cJSON *node_name = cJSON_GetObjectItem(msg, "node_name");

        if (!login || !password || !cJSON_IsString(login) || !cJSON_IsString(password)) {
            ws_send(client_fd, "{\"error\":\"invalid test_account payload\"}");
            return;
        }

        ESP_LOGI("WS", "TEST ACCOUNT login='%s', password='%s'", login->valuestring, password->valuestring);
        // 1️⃣ Обновляем данные авторизации для облака
        strncpy(ws_email, login->valuestring, sizeof(ws_email) - 1);
        strncpy(ws_password, password->valuestring, sizeof(ws_password) - 1);
        strncpy(ws_node_name, node_name->valuestring, sizeof(ws_node_name) - 1);
        test_account_active = true; // включаем режим теста
        // 2️⃣ Останавливаем текущий облачный WS, если есть
        if (client) {
            ESP_LOGW("WS", "🔄 Restarting cloud WebSocket due to new credentials");

            esp_websocket_client_stop(client);
            vTaskDelay(pdMS_TO_TICKS(100));

            esp_websocket_client_destroy(client);
            client = NULL;
        }

        ws_connected = false;                // сброс статуса
        last_ws_event_tick = 0;              // сброс таймера активности
    

        // 3️⃣ Перезапуск WebSocket клиента с новыми данными
        esp_err_t err = websocket_client_start(user.serial,ws_email,ws_password , user.node_name);

        if (err == ESP_OK) {
            ESP_LOGI("WS", "🚀 Cloud WS reconnect started successfully");
        } else {
            ESP_LOGE("WS", "❌ Cloud WS reconnect failed: %s", esp_err_to_name(err));
        }

        ws_send(client_fd, "{\"cloud_status\":\"test_account ok\"}");
        return;
    }


     // === ACTION: cancel_test_account ===
     if (strcmp(act, "cancel_test_account") == 0) {
        ESP_LOGI("WS", "Cancel TEST ACCOUNT, restoring normal credentials");
        cancel_test_account();
        ws_send(client_fd, "{\"cloud_status\":\"test_account cancelled\"}");
        return;
    }

     if (strcmp(act, "wifi_scan") == 0) {
    ESP_LOGI("WS", "WS: wifi_scan request from fd=%d", client_fd);

    wifi_scan_networks();

    ws_send(client_fd, "{\"type\":\"wifi_scan\",\"status\":\"started\"}");
    return;
    }
            if (strcmp(act, "diagnostics_on") == 0) {
                diagnostics_active = true;
                ws_send(client_fd, "{\"diagnostics\":\"on\"}");
                return;
            }

            if (strcmp(act, "diagnostics_off") == 0) {
                diagnostics_active = false;
                ws_send(client_fd, "{\"diagnostics\":\"off\"}");
                return;
            }

    // === НЕИЗВЕСТНОЕ ДЕЙСТВИЕ ===
    else {
        ESP_LOGW("WS", "Unknown WS action '%s'", act);
        ws_send(client_fd, "{\"error\":\"unknown action\"}");
    }
}

// -------------------------------------------------------------------
//  MAIN WS HANDLER
// -------------------------------------------------------------------

/* ------------------------ WEBSOCKET HANDLER ------------------------ */

 esp_err_t ws_handler(httpd_req_t *req)
{
    int client_fd = httpd_req_to_sockfd(req);

     // ==== ЛОГИРОВАНИЕ ВСЕХ WS КЛИЕНТОВ ====
 /*    ESP_LOGI(TAG, "------ WS CLIENT LIST ------");
     for (int i = 0; i < MAX_CLIENTS; i++) {
         if (ws_clients[i].fd != 0) {
             ESP_LOGI(TAG, "slot %d: fd=%d  authorized=%s",
                      i,
                      ws_clients[i].fd,
                      ws_clients[i].authorized ? "true" : "false");
         }
     }
     ESP_LOGI(TAG, "--------------------------------"); */

    // === 1. Если это GET — клиент открывает WebSocket ===
    if (req->method == HTTP_GET) {
        add_client(client_fd);
      //  ESP_LOGI(TAG, "WS client connected, fd=%d", client_fd);
        return ESP_OK;
    }

    // === 2. Получение фрейма ===
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS: recv error (size) fd=%d err=%d", client_fd, ret);
        remove_client(client_fd);
        return ESP_FAIL;
    }

    if (frame.len == 0) {
        // Пустые сообщения игнорируем
        return ESP_OK;
    }

    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS: recv error (payload) fd=%d err=%d", client_fd, ret);
        free(buf);
        remove_client(client_fd);
        return ESP_FAIL;
    }
    buf[frame.len] = 0;

    // === 3. Обработка ping/pong ===
    if (strcmp((char *)buf, "ping") == 0) {
        ws_send(client_fd, "pong");   // отвечаем
        free(buf);
        return ESP_OK;
    }
    if (strcmp((char *)buf, "pong") == 0) {
        // можно обновить timestamp here
        free(buf);
        return ESP_OK;
    }

    // === 4. Парсим JSON ===
    cJSON *msg = cJSON_Parse((char *)buf);
    free(buf);

    if (!msg) {
        ESP_LOGW(TAG, "WS invalid JSON from fd=%d", client_fd);
        return ESP_OK;  // не разрываем связь
    }

    // тип сообщения
    cJSON *type = cJSON_GetObjectItem(msg, "type");
  
    // === 5. Авторизация ===
    if (type && strcmp(type->valuestring, "auth") == 0) {

        cJSON *token = cJSON_GetObjectItem(msg, "token");
        if (token && strcmp(token->valuestring, auth_token) == 0) {

            // авторизация успешна
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (ws_clients[i].fd == client_fd) {
                    ws_clients[i].authorized = true;
                    break;
                }
            }
            network_notify_ws();
          //  ESP_LOGI(TAG, "WS: client authorized fd=%d", client_fd);
        }
        else {
         //   ESP_LOGW(TAG, "WS: unauthorized client fd=%d", client_fd);

            remove_client(client_fd);

            // корректно закрываем WS
            httpd_ws_frame_t close_frame = {
                .final = true,
                .fragmented = false,
                .type = HTTPD_WS_TYPE_CLOSE,
                .payload = NULL,
                .len = 0
            };
            httpd_ws_send_frame_async(server, client_fd, &close_frame);
        }
      
        cJSON_Delete(msg);
        return ESP_OK;
    }

    // === 6. Любые другие сообщения (только если авторизован) ===
    bool allowed = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i].fd == client_fd && ws_clients[i].authorized) {
            allowed = true;
            break;
        }
    }

    if (!allowed) {
        ESP_LOGW(TAG, "WS: message from unauthorized fd=%d", client_fd);
        cJSON_Delete(msg);
        return ESP_OK;
    }

    // здесь твоя обработка WS JSON
    ESP_LOGI(TAG, "WS message fd=%d: %s", client_fd, cJSON_PrintUnformatted(msg));
     handle_ws_custom_message(client_fd, msg);

    cJSON_Delete(msg);
    return ESP_OK;
}



// Функция отмены тестового аккаунта
void cancel_test_account(void) {
    test_account_active = false;
    strncpy(ws_email, user.account_login, sizeof(ws_email)-1);
    strncpy(ws_password, user.account_password, sizeof(ws_password)-1);
    websocket_disable_reconnect();
    // Перезапуск WS с обычным аккаунтом
    if (client) {
        esp_websocket_client_stop(client);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_destroy(client);
        client = NULL;
    }
    websocket_client_start(user.serial, ws_email, ws_password, user.node_name);
    websocket_enable_reconnect();
}

