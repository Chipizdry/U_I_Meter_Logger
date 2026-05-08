


#include "modbus_tcp_client.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MODBUS_TCP";

static modbus_tcp_config_t g_cfg;
static modbus_tcp_callback_t g_callback = NULL;

static uint16_t transaction_id = 0;


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
    int sock;
    struct sockaddr_in dest;

    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(ip);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return ESP_FAIL;

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(sock);
        return ESP_FAIL;
    }

    uint8_t req[32];
    int req_len = build_request(req, unit_id, func, start_addr, quantity, value);

    if (req_len < 0) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    if (send(sock, req, req_len, 0) < 0) {
        close(sock);
        return ESP_FAIL;
    }

    int len = recv(sock, resp, 256, 0);

    if (len <= 0) {
        close(sock);
        return ESP_FAIL;
    }

    *resp_len = len;

    close(sock);
    return ESP_OK;
}



void modbus_tcp_client_start(modbus_tcp_config_t *cfg, modbus_tcp_callback_t cb)
{
    memcpy(&g_cfg, cfg, sizeof(g_cfg));
    g_callback = cb;
}


