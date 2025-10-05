




#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "driver/gpio.h"

#include "ethernet_manager.h"
#include "littlefs_manager.h"
#include "rs485_master.h"

static const char *TAG = "main";

void app_main(void)
{
  //  ESP_LOGI(TAG, "Starting WT32-ETH01 Bridge...");

    vTaskDelay(pdMS_TO_TICKS(2000));

    if (ethernet_init() == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet initialized. Waiting for IP...");

        if (littlefs_init() == ESP_OK) {
            littlefs_write_file("test.txt", "Hello from LittleFS!\n");
            littlefs_list_files();
        }

        rs485_master_init(9600, 2048, 1024);

        rs485_slave_cfg_t s1 = { .slave_addr = 1, .reg_start = 0, .reg_count = 6, .poll_interval_ms = 2000 };
        rs485_master_add_slave(&s1);

        rs485_slave_cfg_t s2 = { .slave_addr = 2, .reg_start = 0, .reg_count = 4, .poll_interval_ms = 5000 };
        rs485_master_add_slave(&s2);
    } else {
        ESP_LOGE(TAG, "Ethernet initialization failed!");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
}
