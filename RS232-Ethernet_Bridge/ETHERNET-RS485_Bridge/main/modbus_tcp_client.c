

#include "modbus_tcp_client.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define TX_SIZE 12
#define RX_SIZE 256

static int build_req(uint8_t *b,
                     uint8_t unit,
                     uint8_t func,
                     uint16_t start,
                     uint16_t qty,
                     uint16_t val)
{
    static uint16_t tx_id = 0;
    tx_id++;

    b[0] = tx_id >> 8;
    b[1] = tx_id & 0xFF;
    b[2] = 0;
    b[3] = 0;

    b[6] = unit;
    b[7] = func;

    b[8] = start >> 8;
    b[9] = start & 0xFF;

    if (func == 3 || func == 4) {
        b[4] = 0;
        b[5] = 6;
        b[10] = qty >> 8;
        b[11] = qty & 0xFF;
        return 12;
    }

    if (func == 6) {
        b[4] = 0;
        b[5] = 6;
        b[10] = val >> 8;
        b[11] = val & 0xFF;
        return 12;
    }

    return -1;
}

esp_err_t modbus_tcp_request(
    const char *ip,
    uint16_t port,
    uint8_t unit,
    uint8_t func,
    uint16_t start,
    uint16_t qty,
    uint16_t value,
    uint8_t *resp,
    int *resp_len)
{
    if (!ip || !resp || !resp_len) return ESP_FAIL;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return ESP_FAIL;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip)
    };

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock);
        return ESP_FAIL;
    }

    uint8_t tx[TX_SIZE];
    uint8_t rx[RX_SIZE];

    int tx_len = build_req(tx, unit, func, start, qty, value);
    if (tx_len < 0) {
        close(sock);
        return ESP_FAIL;
    }

    if (send(sock, tx, tx_len, 0) <= 0) {
        close(sock);
        return ESP_FAIL;
    }

    int len = recv(sock, rx, sizeof(rx), 0);
    if (len <= 6) {
        close(sock);
        return ESP_FAIL;
    }

    // skip MBAP header
    int data_len = len - 6;

    if (data_len > 0) {
        memcpy(resp, &rx[6], data_len);
    }

    *resp_len = data_len;

    close(sock);
    return ESP_OK;
}

