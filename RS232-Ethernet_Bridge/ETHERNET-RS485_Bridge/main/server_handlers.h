


#pragma once
#include "esp_http_server.h"

void register_server_handlers(httpd_handle_t server);

/* individual handlers */
//esp_err_t get_settings_handler(httpd_req_t *req);
//esp_err_t save_settings_post_handler(httpd_req_t *req);
esp_err_t file_get_handler(httpd_req_t *req);
esp_err_t captive_redirect_handler(httpd_req_t *req);
esp_err_t ping_handler(httpd_req_t *req);
esp_err_t options_handler(httpd_req_t *req);
esp_err_t logout_post_handler(httpd_req_t *req);
esp_err_t factory_reset_post_handler(httpd_req_t *req);
esp_err_t reboot_post_handler(httpd_req_t *req);


