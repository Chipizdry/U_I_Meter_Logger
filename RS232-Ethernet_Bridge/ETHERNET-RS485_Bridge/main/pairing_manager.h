


#ifndef PAIRING_MANAGER_H
#define PAIRING_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#define PAIRING_HOLD_TIME_MS   2500
#define RESET_HOLD_TIME_MS     10000
#define PAIRING_TIMEOUT_MS     90000

esp_err_t pairing_manager_request(void);
bool      pairing_is_active(void);

#endif

