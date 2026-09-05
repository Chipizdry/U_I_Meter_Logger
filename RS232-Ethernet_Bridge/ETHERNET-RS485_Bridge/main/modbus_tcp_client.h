#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    MODBUS_TCP_OK = 0,

    MODBUS_TCP_ERR_INVALID_ARG,
    MODBUS_TCP_ERR_SOCKET,
    MODBUS_TCP_ERR_CONNECT,
    MODBUS_TCP_ERR_SEND,

    // TCP соединение установлено,
    // но Modbus slave не ответил за timeout
    MODBUS_TCP_ERR_TIMEOUT,

    // Slave сам закрыл TCP соединение
    MODBUS_TCP_ERR_CONNECTION_CLOSED,

    // Некорректный MBAP
    MODBUS_TCP_ERR_INVALID_MBAP,

    // Некорректный PDU
    MODBUS_TCP_ERR_INVALID_PDU,

    // Modbus exception response
    MODBUS_TCP_ERR_EXCEPTION,

} modbus_tcp_status_t;


modbus_tcp_status_t modbus_tcp_request(
    const char *ip,
    uint16_t port,
    uint8_t unit,
    uint8_t func,
    uint16_t start,
    uint16_t qty,
    uint16_t value,
    uint8_t *resp,
    int *resp_len
);


