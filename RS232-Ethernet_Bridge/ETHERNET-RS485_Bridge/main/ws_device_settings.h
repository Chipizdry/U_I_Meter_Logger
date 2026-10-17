

#pragma once

#include <stdbool.h>
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif

// основной входной обработчик settings сообщений

bool handle_settings_command(cJSON *root);
#ifdef __cplusplus
}
#endif

