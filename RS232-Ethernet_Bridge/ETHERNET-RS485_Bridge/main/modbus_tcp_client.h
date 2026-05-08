


#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef void (*modbus_tcp_callback_t)(const char *hex_response);

typedef struct {
    char server_ip[16];
    uint16_t port;
    uint8_t unit_id;
    uint16_t poll_interval_ms;
    uint16_t start_addr;
    uint16_t quantity;
} modbus_tcp_config_t;

void modbus_tcp_client_start(modbus_tcp_config_t *cfg, modbus_tcp_callback_t cb);

esp_err_t modbus_tcp_request(
    const char *ip,
    uint16_t port,
    uint8_t unit_id,
    uint8_t func,
    uint16_t start_addr,
    uint16_t quantity,
    uint16_t value,
    uint8_t *resp,
    int *resp_len
);

