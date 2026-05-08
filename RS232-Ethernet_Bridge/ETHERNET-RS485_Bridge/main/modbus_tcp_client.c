


#include "modbus_tcp_client.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define MAX_MODBUS_CONNS 3

static const char *TAG = "MODBUS_TCP";

static modbus_tcp_config_t g_cfg;
static modbus_tcp_callback_t g_callback = NULL;

static uint16_t transaction_id = 0;

typedef struct {
    char ip[16];
    uint16_t port;
    int sock;
    uint32_t last_used;
} modbus_conn_t;

static modbus_conn_t conns[MAX_MODBUS_CONNS];

static modbus_conn_t* create_conn(const char *ip, uint16_t port)
{
    for (int i = 0; i < MAX_MODBUS_CONNS; i++) {
        if (conns[i].sock <= 0) {

            struct sockaddr_in dest;
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);
            dest.sin_addr.s_addr = inet_addr(ip);

            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return NULL;

            if (connect(sock, (struct sockaddr*)&dest, sizeof(dest)) != 0) {
                close(sock);
                return NULL;
            }

            strncpy(conns[i].ip, ip, sizeof(conns[i].ip));
            conns[i].port = port;
            conns[i].sock = sock;
            conns[i].last_used = xTaskGetTickCount();

            ESP_LOGI(TAG, "🔌 New Modbus conn %s:%d", ip, port);
            return &conns[i];
        }
    }

    return NULL;
}


static modbus_conn_t* get_conn(const char *ip, uint16_t port)
{
    for (int i = 0; i < MAX_MODBUS_CONNS; i++) {
        if (conns[i].sock > 0 &&
            strcmp(conns[i].ip, ip) == 0 &&
            conns[i].port == port) {
            conns[i].last_used = xTaskGetTickCount();
            return &conns[i];
        }
    }
    return NULL;
}

static modbus_conn_t* modbus_get_connection(const char *ip, uint16_t port)
{
    modbus_conn_t *c = get_conn(ip, port);
    if (c) return c;

    return create_conn(ip, port);
}



static int build_request(uint8_t *buf, uint8_t unit_id, uint8_t func, uint16_t start, uint16_t quantity, uint16_t value)  
{
    transaction_id++;

    buf[0] = transaction_id >> 8;
    buf[1] = transaction_id & 0xFF;

    buf[2] = 0;
    buf[3] = 0;

    buf[6] = unit_id;
    buf[7] = func;

    buf[8] = start >> 8;
    buf[9] = start & 0xFF;

    if (func == 0x03 || func == 0x04) {
        buf[4] = 0;
        buf[5] = 6;

        buf[10] = quantity >> 8;
        buf[11] = quantity & 0xFF;

        return 12;
    }
    else if (func == 0x06) {
        buf[4] = 0;
        buf[5] = 6;

        buf[10] = value >> 8;
        buf[11] = value & 0xFF;

        return 12;
    }

    return -1;
}

esp_err_t modbus_tcp_request( const char *ip,uint16_t port, uint8_t unit_id, uint8_t func, uint16_t start_addr, uint16_t quantity, uint16_t value, uint8_t *resp,int *resp_len)
{
    modbus_conn_t *conn = modbus_get_connection(ip, port);
    if (!conn || conn->sock < 0) {
        return ESP_FAIL;
    }

    uint8_t req[32];
    int req_len = build_request(req, unit_id, func, start_addr, quantity, value);

    if (send(conn->sock, req, req_len, 0) < 0) {
        ESP_LOGW(TAG, "send failed → drop conn");

        close(conn->sock);
        conn->sock = -1;

        return ESP_FAIL;
    }

    int len = recv(conn->sock, resp, 256, 0);

    if (len <= 0) {
        ESP_LOGW(TAG, "recv failed → drop conn");

        close(conn->sock);
        conn->sock = -1;

        return ESP_FAIL;
    }

    *resp_len = len;
    return ESP_OK;
}



void modbus_tcp_client_start(modbus_tcp_config_t *cfg, modbus_tcp_callback_t cb)
{
    memcpy(&g_cfg, cfg, sizeof(g_cfg));
    g_callback = cb;
}


