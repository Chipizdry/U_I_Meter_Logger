

#include "modbus_tcp_client.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "esp_log.h"

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
/*
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
*/
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
    if (!ip || !resp || !resp_len)
        return ESP_ERR_INVALID_ARG;

    *resp_len = 0;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip)
    };

    struct timeval tv = {
        .tv_sec = 2,
        .tv_usec = 0
    };

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               &tv, sizeof(tv));

    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               &tv, sizeof(tv));

    if (connect(sock,
                (struct sockaddr *)&addr,
                sizeof(addr)) != 0)
    {
        close(sock);
        return ESP_FAIL;
    }

    // ---------------------------------------------------------
    // BUILD REQUEST
    // ---------------------------------------------------------

    uint8_t tx[TX_SIZE];

    int tx_len = build_req(
        tx,
        unit,
        func,
        start,
        qty,
        value
    );

    if (tx_len < 0) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI("modbus_tcp",
             "TX %d bytes", tx_len);

    ESP_LOG_BUFFER_HEX("modbus_tcp", tx, tx_len);

    // ---------------------------------------------------------
    // SEND
    // ---------------------------------------------------------

    int sent = send(sock, tx, tx_len, 0);

    if (sent != tx_len) {
        ESP_LOGE("modbus_tcp",
                 "send failed: %d/%d",
                 sent,
                 tx_len);

        close(sock);
        return ESP_FAIL;
    }

    // ---------------------------------------------------------
    // RECEIVE MBAP HEADER
    //
    // Transaction ID  : 2
    // Protocol ID     : 2
    // Length           : 2
    // Unit ID          : 1
    //
    // Total            : 7 bytes
    // ---------------------------------------------------------

    uint8_t mbap[7];

    int received = 0;

    while (received < 7) {

        int n = recv(
            sock,
            mbap + received,
            7 - received,
            0
        );

        if (n <= 0) {
            ESP_LOGE("modbus_tcp",
                     "MBAP recv failed");

            close(sock);
            return ESP_FAIL;
        }

        received += n;
    }

    ESP_LOGI("modbus_tcp",
             "RX MBAP:");

    ESP_LOG_BUFFER_HEX(
        "modbus_tcp",
        mbap,
        7
    );

    // ---------------------------------------------------------
    // VALIDATE MBAP
    // ---------------------------------------------------------

    uint16_t transaction_id =
        ((uint16_t)mbap[0] << 8) | mbap[1];

    uint16_t protocol_id =
        ((uint16_t)mbap[2] << 8) | mbap[3];

    uint16_t length =
        ((uint16_t)mbap[4] << 8) | mbap[5];

    uint8_t rx_unit = mbap[6];

    ESP_LOGI("modbus_tcp",
             "MBAP: TID=%u PID=%u LEN=%u UNIT=%u",
             transaction_id,
             protocol_id,
             length,
             rx_unit);

    if (protocol_id != 0) {
        ESP_LOGE("modbus_tcp",
                 "Invalid protocol ID");

        close(sock);
        return ESP_FAIL;
    }

    if (length < 2) {
        ESP_LOGE("modbus_tcp",
                 "Invalid MBAP length: %u",
                 length);

        close(sock);
        return ESP_FAIL;
    }

    // length includes:
    //
    // Unit ID + PDU
    //
    // Unit ID already received,
    // therefore remaining PDU bytes:

    int pdu_len = length - 1;

    if (pdu_len > RX_SIZE) {
        ESP_LOGE("modbus_tcp",
                 "PDU too large: %d",
                 pdu_len);

        close(sock);
        return ESP_ERR_NO_MEM;
    }

    // ---------------------------------------------------------
    // RECEIVE PDU
    // ---------------------------------------------------------

    received = 0;

    while (received < pdu_len) {

        int n = recv(
            sock,
            resp + received,
            pdu_len - received,
            0
        );

        if (n <= 0) {

            ESP_LOGE("modbus_tcp",
                     "PDU recv failed");

            close(sock);
            return ESP_FAIL;
        }

        received += n;
    }

    *resp_len = received;

    // ---------------------------------------------------------
    // DEBUG
    // ---------------------------------------------------------

    ESP_LOGI("modbus_tcp",
             "RX PDU %d bytes",
             received);

    ESP_LOG_BUFFER_HEX(
        "modbus_tcp",
        resp,
        received
    );

    // ---------------------------------------------------------
    // CLOSE
    // ---------------------------------------------------------

    close(sock);

    return ESP_OK;
}