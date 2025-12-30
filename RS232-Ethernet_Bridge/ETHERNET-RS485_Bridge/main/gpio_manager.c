



#include "gpio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "network_state.h"

static const char *TAG = "GPIO_MANAGER";
static void status_led_task(void *arg);

// ================================
// ========== PRIVATE =============
// ================================
/*
static void reset_to_factory_defaults(void)
{
    ESP_LOGW(TAG, "RESET button held for 5s — restoring factory defaults!");

    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS successfully erased");
    } else {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
*/

static void factory_reset_task(void *arg)
{
    ESP_LOGW(TAG, "Factory reset requested via WEB");

    vTaskDelay(pdMS_TO_TICKS(200)); // дать HTTP ответу уйти

    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS erased successfully");
    } else {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

 void reset_to_factory_defaults(void)
{
    xTaskCreate(
        factory_reset_task,
        "factory_reset_task",
        4096,
        NULL,
        5,
        NULL
    );
}

// ================================
// ========== PUBLIC ==============
// ================================

esp_err_t gpio_manager_init(void)
{
    // ====== Кнопка RESET ======
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << GPIO_RESET_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_conf);

     // Настройка DE-пина
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RS485_DE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // ====== Светодиоды ======
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << GPIO_STATUS_LED) |
                        (1ULL << GPIO_NET_LED) |
                        (1ULL << GPIO_ERROR_LED)|
                        (1ULL << GPIO_MODE_CHANGE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);

    // Гасим все светодиоды
    gpio_set_level(GPIO_STATUS_LED, 0);
    gpio_set_level(GPIO_NET_LED, 0);
    gpio_set_level(GPIO_ERROR_LED, 0);
    gpio_set_level(GPIO_MODE_CHANGE, 0);

    ESP_LOGI(TAG, "GPIOs configured: RESET as input, LEDs as outputs");

    ESP_LOGI(TAG, "Running LED self-test sequence...");
    gpio_led_selftest();


    // Запускаем задачу опроса кнопки
    xTaskCreate(gpio_manager_task, "gpio_manager_task", 2048, NULL, 5, NULL);
    xTaskCreate(status_led_task, "status_led", 2048, NULL, 4, NULL);

    return ESP_OK;
}



void gpio_manager_task(void *arg)
{
    int pressed_time = 0;
    const int step_ms = 100;

    while (1) {
        if (gpio_get_level(GPIO_RESET_BUTTON) == 0) { // активный LOW
            pressed_time += step_ms;

            // Мигаем LED во время удержания
         //   gpio_set_level(GPIO_STATUS_LED, (pressed_time / 250) % 2);

            if (pressed_time >= RESET_HOLD_TIME_MS) {
                reset_to_factory_defaults();
            }
        } else {
            pressed_time = 0;
           // gpio_set_level(GPIO_STATUS_LED, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}


static void status_led_task(void *arg)
{
    bool led = false;

    while (1) {
        net_state_t state = network_get_state();

        switch (state) {
        case NET_STATE_DOWN:
            gpio_set_level(GPIO_STATUS_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case NET_STATE_CONNECTING:
            led = !led;
            gpio_set_level(GPIO_STATUS_LED, led);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        case NET_STATE_UP:
            gpio_set_level(GPIO_STATUS_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }
}

// ================================
// ======= ИНДИКАТОРЫ ============
// ================================

void gpio_set_status_led(bool state)
{
    gpio_set_level(GPIO_STATUS_LED, state);
}


void gpio_set_net_led(bool state)
{
    gpio_set_level(GPIO_NET_LED, state ? 1 : 0);
}


void gpio_blink_status_led(int times, int delay_ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(GPIO_STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        gpio_set_level(GPIO_STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void gpio_indicate_error(void)
{
    for (int i = 0; i < 5; i++) {
        gpio_set_level(GPIO_ERROR_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(GPIO_ERROR_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}



void gpio_led_selftest(void)
{
    const int delay = 450; // мс

    // 1. Поочередно включаем каждый LED

    gpio_set_level(RS485_DE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(delay));
    gpio_set_level(RS485_DE_PIN, 0);

    gpio_set_level(GPIO_MODE_CHANGE, 1);
    vTaskDelay(pdMS_TO_TICKS(delay));
    gpio_set_level(GPIO_MODE_CHANGE, 0);

    gpio_set_level(GPIO_NET_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(delay));
    gpio_set_level(GPIO_NET_LED, 0);

    gpio_set_level(GPIO_STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(delay));
    gpio_set_level(GPIO_STATUS_LED, 0);

    gpio_set_level(GPIO_ERROR_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(delay));
    gpio_set_level(GPIO_ERROR_LED, 0);


    // 2. Все вместе — короткая вспышка
    gpio_set_level(GPIO_STATUS_LED, 1);
    gpio_set_level(GPIO_NET_LED, 1);
    gpio_set_level(GPIO_ERROR_LED, 1);
    gpio_set_level(GPIO_MODE_CHANGE, 1);
    gpio_set_level(RS485_DE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(400));
    gpio_set_level(GPIO_STATUS_LED, 0);
    gpio_set_level(GPIO_NET_LED, 0);
    gpio_set_level(GPIO_ERROR_LED, 0);
    gpio_set_level(GPIO_MODE_CHANGE, 0);
    gpio_set_level(RS485_DE_PIN, 0);

    ESP_LOGI(TAG, "LED self-test completed");
}
