

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t modbus_tcp_request(
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

#ifdef __cplusplus
}
#endif

