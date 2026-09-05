#include "modbus_tcp_client.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "esp_log.h"


#define TAG "modbus_tcp"

#define TX_SIZE 12
#define RX_SIZE 256

#define MODBUS_TCP_TIMEOUT_MS 2000


// ============================================================================
// BUILD MODBUS TCP REQUEST
// ============================================================================

static int build_req(uint8_t *b,
                     uint8_t unit,
                     uint8_t func,
                     uint16_t start,
                     uint16_t qty,
                     uint16_t val)
{
    static uint16_t tx_id = 0;

    tx_id++;

    /*
     * MBAP
     *
     * Transaction ID : 2
     * Protocol ID    : 2
     * Length         : 2
     * Unit ID        : 1
     */

    b[0] = tx_id >> 8;
    b[1] = tx_id & 0xFF;

    b[2] = 0;
    b[3] = 0;

    b[6] = unit;

    // PDU
    b[7] = func;

    b[8] = start >> 8;
    b[9] = start & 0xFF;


    // ---------------------------------------------------------
    // FC03 / FC04 - Read Holding/Input Registers
    // ---------------------------------------------------------

    if (func == 3 || func == 4) {

        // Unit ID + Function + Start + Quantity
        // = 1 + 1 + 2 + 2 = 6
        b[4] = 0;
        b[5] = 6;

        b[10] = qty >> 8;
        b[11] = qty & 0xFF;

        return 12;
    }


    // ---------------------------------------------------------
    // FC06 - Write Single Register
    // ---------------------------------------------------------

    if (func == 6) {

        // Unit ID + Function + Address + Value
        // = 1 + 1 + 2 + 2 = 6
        b[4] = 0;
        b[5] = 6;

        b[10] = val >> 8;
        b[11] = val & 0xFF;

        return 12;
    }


    return -1;
}


// ============================================================================
// RECEIVE EXACTLY N BYTES
//
// Return:
//   >0  = bytes received
//    0  = remote closed connection
//   -1  = socket error
//   -2  = timeout
// ============================================================================

static int recv_exact(int sock,
                      uint8_t *buf,
                      int len)
{
    int received = 0;

    while (received < len) {

        int n = recv(
            sock,
            buf + received,
            len - received,
            0
        );


        // -----------------------------------------------------
        // TIMEOUT / SOCKET ERROR
        // -----------------------------------------------------

        if (n < 0) {

            if (errno == EAGAIN ||
                errno == EWOULDBLOCK) {

                return -2;
            }

            return -1;
        }


        // -----------------------------------------------------
        // REMOTE CLOSED CONNECTION
        // -----------------------------------------------------

        if (n == 0) {

            return 0;
        }


        received += n;
    }

    return received;
}


// ============================================================================
// MODBUS TCP REQUEST
// ============================================================================

modbus_tcp_status_t modbus_tcp_request(
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
    // ------------------------------------------------------------------------
    // ARGUMENT CHECK
    // ------------------------------------------------------------------------

    if (!ip || !resp || !resp_len) {

        ESP_LOGE(
            TAG,
            "Invalid argument"
        );

        return MODBUS_TCP_ERR_INVALID_ARG;
    }


    *resp_len = 0;


    // ------------------------------------------------------------------------
    // CREATE SOCKET
    // ------------------------------------------------------------------------

    int sock = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (sock < 0) {

        ESP_LOGE(
            TAG,
            "socket() failed: errno=%d (%s)",
            errno,
            strerror(errno)
        );

        return MODBUS_TCP_ERR_SOCKET;
    }


    // ------------------------------------------------------------------------
    // SERVER ADDRESS
    // ------------------------------------------------------------------------

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);


    // ------------------------------------------------------------------------
    // SOCKET TIMEOUT
    // ------------------------------------------------------------------------

    struct timeval tv = {
        .tv_sec = MODBUS_TCP_TIMEOUT_MS / 1000,
        .tv_usec = (MODBUS_TCP_TIMEOUT_MS % 1000) * 1000
    };


    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &tv,
            sizeof(tv)) < 0) {

        ESP_LOGW(
            TAG,
            "SO_RCVTIMEO failed: errno=%d",
            errno
        );
    }


    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &tv,
            sizeof(tv)) < 0) {

        ESP_LOGW(
            TAG,
            "SO_SNDTIMEO failed: errno=%d",
            errno
        );
    }


    // ------------------------------------------------------------------------
    // CONNECT
    // ------------------------------------------------------------------------

    ESP_LOGI(
        TAG,
        "Connecting to %s:%u",
        ip,
        port
    );


    if (connect(
            sock,
            (struct sockaddr *)&addr,
            sizeof(addr)) != 0) {

        ESP_LOGW(
            TAG,
            "connect() failed: %s:%u errno=%d (%s)",
            ip,
            port,
            errno,
            strerror(errno)
        );

        close(sock);

        return MODBUS_TCP_ERR_CONNECT;
    }


    ESP_LOGI(
        TAG,
        "Connected to %s:%u",
        ip,
        port
    );


    // ------------------------------------------------------------------------
    // BUILD REQUEST
    // ------------------------------------------------------------------------

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

        ESP_LOGE(
            TAG,
            "Unsupported Modbus function: %u",
            func
        );

        close(sock);

        return MODBUS_TCP_ERR_INVALID_ARG;
    }


    ESP_LOGI(
        TAG,
        "TX %d bytes",
        tx_len
    );

    ESP_LOG_BUFFER_HEX(
        TAG,
        tx,
        tx_len
    );


    // ------------------------------------------------------------------------
    // SEND REQUEST
    // ------------------------------------------------------------------------

    int sent = send(
        sock,
        tx,
        tx_len,
        0
    );


    if (sent < 0) {

        ESP_LOGW(
            TAG,
            "send() failed: errno=%d (%s)",
            errno,
            strerror(errno)
        );

        close(sock);

        return MODBUS_TCP_ERR_SEND;
    }


    if (sent != tx_len) {

        ESP_LOGW(
            TAG,
            "Partial send: %d/%d",
            sent,
            tx_len
        );

        close(sock);

        return MODBUS_TCP_ERR_SEND;
    }


    // ------------------------------------------------------------------------
    // RECEIVE MBAP
    //
    // Transaction ID : 2
    // Protocol ID    : 2
    // Length         : 2
    // Unit ID        : 1
    //
    // Total = 7 bytes
    // ------------------------------------------------------------------------

    uint8_t mbap[7];

    int n = recv_exact(
        sock,
        mbap,
        sizeof(mbap)
    );


    // ------------------------------------------------------------------------
    // RECEIVE ERROR
    // ------------------------------------------------------------------------

    if (n == -2) {

        ESP_LOGW(
            TAG,
            "TIMEOUT waiting MBAP response from %s:%u",
            ip,
            port
        );

        close(sock);

        return MODBUS_TCP_ERR_TIMEOUT;
    }


    if (n == 0) {

        ESP_LOGW(
            TAG,
            "Remote host closed connection without MBAP: %s:%u",
            ip,
            port
        );

        close(sock);

        return MODBUS_TCP_ERR_CONNECTION_CLOSED;
    }


    if (n < 0) {

        ESP_LOGW(
            TAG,
            "MBAP recv socket error: errno=%d (%s)",
            errno,
            strerror(errno)
        );

        close(sock);

        return MODBUS_TCP_ERR_SOCKET;
    }


    // ------------------------------------------------------------------------
    // DEBUG MBAP
    // ------------------------------------------------------------------------

    ESP_LOGI(
        TAG,
        "RX MBAP"
    );

    ESP_LOG_BUFFER_HEX(
        TAG,
        mbap,
        sizeof(mbap)
    );


    // ------------------------------------------------------------------------
    // PARSE MBAP
    // ------------------------------------------------------------------------

    uint16_t transaction_id =
        ((uint16_t)mbap[0] << 8) |
        mbap[1];


    uint16_t protocol_id =
        ((uint16_t)mbap[2] << 8) |
        mbap[3];


    uint16_t length =
        ((uint16_t)mbap[4] << 8) |
        mbap[5];


    uint8_t rx_unit = mbap[6];


    ESP_LOGI(
        TAG,
        "MBAP: TID=%u PID=%u LEN=%u UNIT=%u",
        transaction_id,
        protocol_id,
        length,
        rx_unit
    );


    // ------------------------------------------------------------------------
    // VALIDATE PROTOCOL ID
    // ------------------------------------------------------------------------

    if (protocol_id != 0) {

        ESP_LOGW(
            TAG,
            "Invalid Modbus protocol ID: %u",
            protocol_id
        );

        close(sock);

        return MODBUS_TCP_ERR_INVALID_MBAP;
    }


    // ------------------------------------------------------------------------
    // VALIDATE LENGTH
    //
    // Length = Unit ID + PDU
    // Minimum = 2
    // ------------------------------------------------------------------------

    if (length < 2) {

        ESP_LOGW(
            TAG,
            "Invalid MBAP length: %u",
            length
        );

        close(sock);

        return MODBUS_TCP_ERR_INVALID_MBAP;
    }


    // ------------------------------------------------------------------------
    // CHECK UNIT ID
    // ------------------------------------------------------------------------

    if (rx_unit != unit) {

        ESP_LOGW(
            TAG,
            "Unit ID mismatch: TX=%u RX=%u",
            unit,
            rx_unit
        );

        close(sock);

        return MODBUS_TCP_ERR_INVALID_MBAP;
    }


    // ------------------------------------------------------------------------
    // CHECK RESPONSE SIZE
    // ------------------------------------------------------------------------

    int pdu_len = length - 1;


    if (pdu_len <= 0 ||
        pdu_len > RX_SIZE) {

        ESP_LOGW(
            TAG,
            "Invalid PDU length: %d",
            pdu_len
        );

        close(sock);

        return MODBUS_TCP_ERR_INVALID_MBAP;
    }


    // ------------------------------------------------------------------------
    // RECEIVE PDU
    // ------------------------------------------------------------------------

    n = recv_exact(
        sock,
        resp,
        pdu_len
    );


    // ------------------------------------------------------------------------
    // PDU RECEIVE ERROR
    // ------------------------------------------------------------------------

    if (n == -2) {

        ESP_LOGW(
            TAG,
            "TIMEOUT waiting PDU response from %s:%u",
            ip,
            port
        );

        close(sock);

        return MODBUS_TCP_ERR_TIMEOUT;
    }


    if (n == 0) {

        ESP_LOGW(
            TAG,
            "Remote host closed connection during PDU",
            ip
        );

        close(sock);

        return MODBUS_TCP_ERR_CONNECTION_CLOSED;
    }


    if (n < 0) {

        ESP_LOGW(
            TAG,
            "PDU recv socket error: errno=%d (%s)",
            errno,
            strerror(errno)
        );

        close(sock);

        return MODBUS_TCP_ERR_SOCKET;
    }


    *resp_len = n;


    // ------------------------------------------------------------------------
    // DEBUG PDU
    // ------------------------------------------------------------------------

    ESP_LOGI(
        TAG,
        "RX PDU %d bytes",
        n
    );

    ESP_LOG_BUFFER_HEX(
        TAG,
        resp,
        n
    );


    // ------------------------------------------------------------------------
    // VALIDATE FUNCTION CODE
    // ------------------------------------------------------------------------

    uint8_t rx_func = resp[0];


    // Modbus exception response:
    //
    // requested function = 03
    // response function  = 83
    //

    if (rx_func == (uint8_t)(func | 0x80)) {

        uint8_t exception_code =
            (n >= 2) ? resp[1] : 0;

        ESP_LOGW(
            TAG,
            "Modbus exception: func=%u code=%u",
            func,
            exception_code
        );

        close(sock);

        return MODBUS_TCP_ERR_EXCEPTION;
    }


    // ------------------------------------------------------------------------
    // FUNCTION CODE MUST MATCH
    // ------------------------------------------------------------------------

    if (rx_func != func) {

        ESP_LOGW(
            TAG,
            "Invalid response function: TX=%u RX=%u",
            func,
            rx_func
        );

        close(sock);

        return MODBUS_TCP_ERR_INVALID_PDU;
    }


    // ------------------------------------------------------------------------
    // CLOSE
    // ------------------------------------------------------------------------

    close(sock);


    return MODBUS_TCP_OK;
}


