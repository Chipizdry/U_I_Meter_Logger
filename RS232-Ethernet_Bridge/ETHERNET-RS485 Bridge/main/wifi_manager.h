


#pragma once
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_settings.h"
/*
typedef struct {
    wifi_mode_t mode;
    char ssid[32];
    char password[64];
    char ap_ssid[32];
    char ap_password[64];
    int  ap_channel;
} wifi_settings_t;

*/

esp_err_t wifi_manager_init(const wifi_settings_t *cfg);
void wifi_manager_stop(void);


