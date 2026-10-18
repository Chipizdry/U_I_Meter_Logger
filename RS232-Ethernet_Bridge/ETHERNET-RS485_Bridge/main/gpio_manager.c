



#include "gpio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_settings.h"
#include "network_state.h"
#include "pairing_manager.h"
#include "websocket_client.h" 

static const char *TAG = "GPIO_MANAGER";
static void status_led_task(void *arg);

typedef enum {
    HOLD_NONE = 0,        /* кнопка не нажата */
    HOLD_PROGRESS,        /* удержание < PAIRING_HOLD_TIME_MS — «накапливаем» */
    HOLD_PAIRING_READY,   /* ≥ PAIRING_HOLD_TIME_MS — отпусти → pairing */
    HOLD_RESET_READY,     /* ≥ RESET_HOLD_TIME_MS — отпусти → factory reset */
} hold_state_t;

static volatile hold_state_t s_hold_state = HOLD_NONE;

// ================================
// ========== PRIVATE =============
// ================================

static void factory_reset_task(void *arg)
{
    ESP_LOGW(TAG, "Factory reset requested via WEB");

    vTaskDelay(pdMS_TO_TICKS(200)); // дать HTTP ответу уйти
     nvs_factory_reset();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}


 void reset_to_factory_defaults(void)
{
    xTaskCreate(factory_reset_task,"factory_reset_task",4096,NULL,5,NULL);
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

    //====== Сброс ETHERNET=======
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << ETH_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&rst_conf);


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
                        (1ULL << GPIO_LINK_INDICATOR) |
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
    gpio_set_level(GPIO_LINK_INDICATOR, 0);
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
    bool button_was_pressed = false;
    bool action_done = false;

    while (1) {
        bool pressed = (gpio_get_level(GPIO_RESET_BUTTON) == 0);

        if (pressed) {
            button_was_pressed = true;

            if (pressed_time < RESET_HOLD_TIME_MS) {
                pressed_time += step_ms;
            }

            /* ← NEW: обновляем состояние удержания прямо здесь */
            if (pressed_time >= RESET_HOLD_TIME_MS) {
                s_hold_state = HOLD_RESET_READY;
            } else if (pressed_time >= PAIRING_HOLD_TIME_MS) {
                s_hold_state = HOLD_PAIRING_READY;
            } else if (pressed_time > 0) {
                s_hold_state = HOLD_PROGRESS;
            }
        } else {
            if (button_was_pressed && !action_done) {
                if (pressed_time >= RESET_HOLD_TIME_MS) {
                    action_done = true;
                    reset_to_factory_defaults();
                } else if (pressed_time >= PAIRING_HOLD_TIME_MS) {
                    action_done = true;

                    esp_err_t err = pairing_manager_request();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Pairing request failed: %s",
                                 esp_err_to_name(err));
                    }
                }
            }

            /* ← NEW: сбрасываем состояние сразу после отпускания */
            s_hold_state    = HOLD_NONE;
            pressed_time    = 0;
            button_was_pressed = false;
            action_done     = false;
        }

        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}

static void status_led_task(void *arg)
{
    bool led = false;
    bool was_show_mode = false;   // были ли в hold/pairing на прошлой итерации

    while (1) {
        hold_state_t hold = s_hold_state;
        bool pairing = pairing_is_active();
        bool show_mode = (hold != HOLD_NONE) || pairing;

        /* ---- Выход из «показного» режима — восстановить NET_LED по факту авторизации ---- */
        if (was_show_mode && !show_mode) {
            bool auth = cloud_is_authenticated();
            gpio_set_level(GPIO_NET_LED, auth ? 1 : 0);
           // ESP_LOGI(TAG, "NET_LED restored after hold/pairing: %d (cloud=%s)",auth, cloud_status_msg);
        }
        was_show_mode = show_mode;

        /* ========== 0. Прогресс удержания кнопки — только в этом режиме трогаем NET_LED ========== */
        switch (hold) {
        case HOLD_PROGRESS:
            led = !led;
            gpio_set_level(GPIO_STATUS_LED, led);
            gpio_set_level(GPIO_NET_LED, 0);
            gpio_set_level(GPIO_ERROR_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;

        case HOLD_PAIRING_READY:
            led = !led;
            gpio_set_level(GPIO_STATUS_LED, led);
            gpio_set_level(GPIO_NET_LED, !led);
            gpio_set_level(GPIO_ERROR_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;

        case HOLD_RESET_READY:
            led = !led;
            gpio_set_level(GPIO_STATUS_LED, led);
            gpio_set_level(GPIO_ERROR_LED, !led);
            gpio_set_level(GPIO_NET_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;

        case HOLD_NONE:
        default:
            break;
        }

        /* ========== 1. Режим pairing — мигаем NET_LED как раньше ========== */
        if (pairing) {
            led = !led;
            gpio_set_level(GPIO_STATUS_LED, led);
            gpio_set_level(GPIO_NET_LED, !led);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        /* ========== 2. IDLE: НИКОГДА не трогаем GPIO_NET_LED ==========
         * Владелец NET_LED в idle — только websocket_events/websocket_client.
         * Здесь управляем только STATUS_LED и ERROR_LED.
         */
        net_state_t wifi = network_get_wifi_state();
        net_state_t eth  = network_get_ethernet_state();

        if (wifi == NET_STATE_WIFI_UP || eth == NET_STATE_ETHERNET_UP) {
            gpio_set_level(GPIO_STATUS_LED, 1);
            gpio_set_level(GPIO_ERROR_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else if (wifi == NET_STATE_WIFI_CONNECTING ||
                 eth  == NET_STATE_ETHERNET_CONNECTING) {
            led = !led;
            gpio_set_level(GPIO_STATUS_LED, led);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        else {
            gpio_set_level(GPIO_STATUS_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
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

void gpio_link_led(bool state)
{

 gpio_set_level(GPIO_LINK_INDICATOR, state ? 1 : 0);

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

    gpio_set_level(GPIO_LINK_INDICATOR,1);
    vTaskDelay(pdMS_TO_TICKS(delay));
    gpio_set_level(GPIO_LINK_INDICATOR, 0);


    // 2. Все вместе — короткая вспышка
    gpio_set_level(GPIO_STATUS_LED, 1);
    gpio_set_level(GPIO_NET_LED, 1);
    gpio_set_level(GPIO_ERROR_LED, 1);
    gpio_set_level(GPIO_MODE_CHANGE, 1);
    gpio_set_level(RS485_DE_PIN, 1);
    gpio_set_level(GPIO_LINK_INDICATOR , 1);
    vTaskDelay(pdMS_TO_TICKS(400));
    gpio_set_level(GPIO_STATUS_LED, 0);
    gpio_set_level(GPIO_NET_LED, 0);
    gpio_set_level(GPIO_ERROR_LED, 0);
    gpio_set_level(GPIO_MODE_CHANGE, 0);
    gpio_set_level(RS485_DE_PIN, 0);
    gpio_set_level(GPIO_LINK_INDICATOR , 0);

    ESP_LOGI(TAG, "LED self-test completed");
}
