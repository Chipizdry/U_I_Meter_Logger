




#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_manager_init(void);

/** Запустить GATT pairing-сервис + advertising. */
esp_err_t ble_start_pairing_mode(void);

/** Остановить BLE (после завершения pairing). */
void      ble_stop_pairing_mode(void);

/** Приложение записало ID и получило токен — обмен завершён. */
bool      ble_pairing_is_done(void);

/** Обмен завершён успешно (токен сохранён в NVS). */
bool      ble_pairing_was_successful(void);

#ifdef __cplusplus
}
#endif
