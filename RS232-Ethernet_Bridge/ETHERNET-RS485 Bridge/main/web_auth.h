

#pragma once
#include "esp_http_server.h"
#include <stdbool.h>

extern char auth_token[64];

void generate_token(char *buf, size_t len);
bool check_token(httpd_req_t *req);

// Хендлер логина
esp_err_t login_post_handler(httpd_req_t *req);


