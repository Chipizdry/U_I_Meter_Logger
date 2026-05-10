

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// основной входной обработчик settings сообщений
bool handle_settings_command(const char *json);

#ifdef __cplusplus
}
#endif

