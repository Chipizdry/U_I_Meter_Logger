



#pragma once

#include "esp_err.h"
#include "esp_http_server.h"  
#include <stdbool.h>
// Инициализация HTTP сервера (отдаёт файлы из LittleFS)
esp_err_t web_server_start(void);

// Остановка сервера
esp_err_t web_server_stop(void);
extern char auth_token[64];
bool check_token(httpd_req_t *req);
void fill_netif_ip_info(const char *ifkey,char *ip,char *mask,char *gw,char *dns,size_t len);