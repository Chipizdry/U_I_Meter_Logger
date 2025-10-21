

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USER_SETTINGS_VERSION     1
#define NETWORK_SETTINGS_VERSION  2
#define SYSTEM_SETTINGS_VERSION   1

typedef struct {
    uint8_t version;
    char login[64];
    char password[64];
    char language[8];
} user_settings_t;

typedef struct {
    uint8_t version;
    char ip[16];
    char gateway[16];
    char mask[16];
    char dns[16];
    int port;
    bool dhcp_enabled;
    char ssid[64];
    char mode[16]; 
} network_settings_t;

typedef struct {
    uint8_t version;
    int refresh_interval;
    int log_level;
    bool debug_mode;
} system_settings_t;

// Инициализация NVS
esp_err_t nvs_settings_init(void);

// ======== Пользовательские настройки ========
esp_err_t nvs_save_user_settings(const user_settings_t *settings);
esp_err_t nvs_load_user_settings(user_settings_t *settings);
esp_err_t nvs_clear_user_settings(void);

// ======== Сетевые настройки ========
esp_err_t nvs_save_network_settings(const network_settings_t *settings);
esp_err_t nvs_load_network_settings(network_settings_t *settings);
esp_err_t nvs_clear_network_settings(void);

// ======== Системные настройки ========
esp_err_t nvs_save_system_settings(const system_settings_t *settings);
esp_err_t nvs_load_system_settings(system_settings_t *settings);
esp_err_t nvs_clear_system_settings(void);

#ifdef __cplusplus
}
#endif
