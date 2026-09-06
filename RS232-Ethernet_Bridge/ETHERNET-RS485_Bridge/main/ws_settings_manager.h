

#ifndef WS_SETTINGS_MANAGER_H
#define WS_SETTINGS_MANAGER_H

#include "cJSON.h"
#include "esp_err.h"
#include "nvs_settings.h"
#include "driver/uart.h"

#include "rs485_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_settings_save_account(const user_settings_t *cfg);
esp_err_t ws_settings_save_uart(const uart_settings_t *cfg);

esp_err_t ws_settings_apply_account(cJSON *account, user_settings_t *cfg);
esp_err_t ws_settings_apply_uart(cJSON *uart, uart_settings_t *cfg);
esp_err_t ws_settings_apply_user(cJSON *user_item, user_settings_t *cfg);
#ifdef __cplusplus
}
#endif

#endif /* WS_SETTINGS_MANAGER_H */

