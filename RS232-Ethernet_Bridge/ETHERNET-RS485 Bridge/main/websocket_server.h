


#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"


#ifdef __cplusplus
extern "C" {
#endif

esp_err_t websocket_server_start(void);
esp_err_t websocket_server_broadcast(const char *msg);
esp_err_t websocket_server_send(httpd_req_t *req, const char *msg);
int websocket_server_get_client_count(void);

#ifdef __cplusplus
}
#endif



