

#pragma once
#include "esp_http_server.h"
#include "esp_err.h"

void ws_server_init(httpd_handle_t httpd);

//void ws_send(int fd, const char *text);
void ws_broadcast(const char *text);
void ws_send(int client_fd, const char* text);
void ws_send_fd(int fd, const char *msg);
void ws_cleanup_all_clients(void);
void ws_notify_reconnect(void);
esp_err_t ws_handler(httpd_req_t *req);

extern bool diagnostics_active;