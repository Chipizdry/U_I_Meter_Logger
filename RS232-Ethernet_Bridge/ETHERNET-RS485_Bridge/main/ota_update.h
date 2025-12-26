


#pragma once
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// Инициализация OTA
void ota_init(void);

// HTTP обработчик OTA POST
esp_err_t ota_post_handler(httpd_req_t *req);
esp_err_t fs_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif


