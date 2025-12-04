


#pragma once
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_settings.h"

esp_err_t wifi_manager_init(const wifi_settings_t *cfg);
void wifi_manager_stop(void);


