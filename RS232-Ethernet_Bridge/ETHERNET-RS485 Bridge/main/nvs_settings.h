

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"

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
    char account_login[32];
    char account_password[32];
    char language[8];
    char serial[16];
} user_settings_t;

typedef struct {
    uint8_t version;
    char ip[16];
    char gateway[16];
    char mask[16];
    char dns[16];
    int port;
    bool dhcp_enabled;
    char sta_ssid[64];
    char sta_password[32];
    char ap_ssid[64];
    char ap_password[32];
    wifi_mode_t mode;  

} network_settings_t;

typedef struct {
    uint8_t version;
    int refresh_interval;
    int log_level;
    bool debug_mode;
    int build_number;          // 👈 номер сборки
    char build_date[32];       // 👈 дата и время сборки
} system_settings_t;

typedef struct {
    
    int      baud_rate;
    uint8_t  data_bits;
    uint8_t  stop_bits;
    char     parity;        // 'N', 'E', 'O'
    bool     flow_control;
} uart_settings_t;

extern user_settings_t user;
extern network_settings_t net;
extern system_settings_t sys;
extern uart_settings_t uart_cfg;


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

// ======== UART настройки ========
esp_err_t nvs_save_uart_settings(const uart_settings_t *settings);
esp_err_t nvs_load_uart_settings(uart_settings_t *settings);
esp_err_t nvs_clear_uart_settings(void);


#ifdef __cplusplus
}
#endif
