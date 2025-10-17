


#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Инициализация NVS для хранения учетных данных
esp_err_t nvs_credentials_init(void);

// Сохранение логина и пароля
esp_err_t nvs_save_credentials(const char *login, const char *password);

// Загрузка логина и пароля
esp_err_t nvs_load_credentials(char *login, size_t login_len, char *password, size_t password_len);

// Очистка логина и пароля
esp_err_t nvs_clear_credentials(void);

#ifdef __cplusplus
}
#endif


