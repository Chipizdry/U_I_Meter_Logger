




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
#include "web_server.h"
#include "websocket_client.h"

static const char *TAG = "main";

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

  if (littlefs_init() == ESP_OK) {
    littlefs_list_files();
   }
 

    if (ethernet_init() == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet initialized. Waiting for IP...");

      

        rs485_master_init(9600, 2048, 1024);

        rs485_slave_cfg_t s1 = { .slave_addr = 9, .reg_start = 0, .reg_count = 16, .poll_interval_ms = 500 };
        rs485_master_add_slave(&s1);

       // rs485_slave_cfg_t s2 = { .slave_addr = 2, .reg_start = 0, .reg_count = 4, .poll_interval_ms = 5000 };
       // rs485_master_add_slave(&s2);
    } else {
        ESP_LOGE(TAG, "Ethernet initialization failed!");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    if (web_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    initialize_sntp();


   
    websocket_client_start("123", "chipizdry@gmail.com", "12345678");
}
