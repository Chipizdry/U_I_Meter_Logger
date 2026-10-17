


#ifndef PAIRING_MANAGER_H
#define PAIRING_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#define PAIRING_HOLD_TIME_MS   3000
#define RESET_HOLD_TIME_MS     8000
#define PAIRING_TIMEOUT_MS     60000

esp_err_t pairing_manager_request(void);
bool      pairing_is_active(void);

#endif

