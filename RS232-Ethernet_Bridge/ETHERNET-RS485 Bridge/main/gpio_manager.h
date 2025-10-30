


#ifndef GPIO_MANAGER_H
#define GPIO_MANAGER_H

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===== Пины =====
#define GPIO_RESET_BUTTON   32   // Кнопка Reset
#define GPIO_STATUS_LED     2   // Светодиод статуса
#define GPIO_NET_LED        4   // Светодиод сети 
#define GPIO_ERROR_LED      5   // Светодиод ошибок 

// ===== Время удержания для сброса =====
#define RESET_HOLD_TIME_MS  5000

// ===== Публичные функции =====
esp_err_t gpio_manager_init(void);
void gpio_manager_task(void *arg);

void gpio_set_status_led(bool state);
void gpio_blink_status_led(int times, int delay_ms);
void gpio_indicate_error(void);

#ifdef __cplusplus
}
#endif

#endif // GPIO_MANAGER_H



