


#pragma once
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_settings.h"

esp_err_t wifi_manager_init(const wifi_settings_t *cfg);
void wifi_manager_stop(void);
esp_err_t wifi_scan_networks(void);

 void wifi_manager_task(void *arg);
 esp_err_t wifi_manager_request_apply(void);
 void start_wifi_manager_task(void);
 void send_pending_scan_results(void);