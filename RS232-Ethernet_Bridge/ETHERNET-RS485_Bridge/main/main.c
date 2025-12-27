




#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_system.h"

#include "ethernet_manager.h"
#include "littlefs_manager.h"
#include "rs485_master.h"
#include "web_server.h"
#include "websocket_client.h"
#include "nvs_settings.h"
#include "wifi_manager.h"
#include "gpio_manager.h"
#include "ota_update.h"
#include "dns_server.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static const char *TAG = "main";

char login[64];
char password[64];

user_settings_t user;
network_settings_t net;
wifi_settings_t wifi_cfg;
system_settings_t sys;
uart_settings_t uart_cfg;

void app_main(void)
{
   

    const esp_partition_t *running    = esp_ota_get_running_partition();
    const esp_partition_t *boot       = esp_ota_get_boot_partition();
    const esp_partition_t *next       = esp_ota_get_next_update_partition(NULL);

    ESP_LOGW("BOOT", "========== BOOT INFO ==========");
    ESP_LOGW("BOOT", "Running partition  : %s", running->label);
    ESP_LOGW("BOOT", "Configured boot    : %s", boot->label);
    ESP_LOGW("BOOT", "Next OTA target    : %s", next ? next->label : "NULL");
    ESP_LOGW("BOOT", "Running addr       : 0x%lx", running->address);
    ESP_LOGW("BOOT", "Running size       : 0x%lx", running->size);
    ESP_LOGW("BOOT", "==============================");

    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_ERROR_CHECK(nvs_settings_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
   
    nvs_load_user_settings(&user);
    nvs_load_network_settings(&net);
    nvs_load_wifi_settings(&wifi_cfg);
    nvs_load_system_settings(&sys);
    nvs_load_uart_settings(&uart_cfg);

    
    /* === АВТО-ОБНОВЛЕНИЕ BUILD INFO ПРИ НОВОЙ ПРОШИВКЕ === */
    if (sys.build_number != BUILD_NUMBER) {
        ESP_LOGW("SYS", "New firmware detected!");
        ESP_LOGW("SYS", "Old build: %d (%s)", sys.build_number, sys.build_date);
        ESP_LOGW("SYS", "New build: %d (%s)", BUILD_NUMBER, BUILD_DATE);

        sys.build_number = BUILD_NUMBER;
        strncpy(sys.build_date, BUILD_DATE, sizeof(sys.build_date) - 1);
        sys.build_date[sizeof(sys.build_date) - 1] = 0;

        ESP_ERROR_CHECK(nvs_save_system_settings(&sys));
    }

    ESP_LOGI("MAIN", "User: %s / %s (%s) SN=%s Name=%s Account:%s ? Pass:%s",user.login, user.password, user.language, user.serial, user.node_name , user.account_login, user.account_password);
    ESP_LOGI("MAIN", "Network: IP=%s DHCP=%d", net.ip, net.dhcp_enabled);
    ESP_LOGI("MAIN", "WiFi Settings:");
    ESP_LOGI("MAIN", "Mode: %s", wifi_cfg.mode == WIFI_MODE_STA   ? "STA" :wifi_cfg.mode == WIFI_MODE_AP    ? "AP" : wifi_cfg.mode == WIFI_MODE_APSTA ? "AP+STA" : "UNKNOWN");
    ESP_LOGI("MAIN", "STA: SSID=%s  PASS=%s", wifi_cfg.sta_ssid, wifi_cfg.sta_password);
    ESP_LOGI("MAIN", "AP : SSID=%s  PASS=%s  Channel=%d " , wifi_cfg.ap_ssid, wifi_cfg.ap_password, wifi_cfg.ap_channel);
    ESP_LOGI("MAIN", "System: refresh=%dms log=%d debug=%d build=%d (%s)",sys.refresh_interval, sys.log_level, sys.debug_mode,sys.build_number, sys.build_date);
    ESP_LOGI("MAIN", "Loaded UART config: baudrate:%d , DataBits:%d ,StopBits:%d ,Parity: %d, Mode:(%s) ", uart_cfg.baud_rate, uart_cfg.data_bits, uart_cfg.stop_bits, uart_cfg.parity, uart_cfg.rs485_mode ? "RS485" : "RS232");

    ESP_ERROR_CHECK(gpio_manager_init());

    if (littlefs_init() == ESP_OK) {
        littlefs_list_files();
    }
 
    rs485_master_init_from_cfg(&uart_cfg, 2048, 1024);

    rs485_slave_cfg_t s1 = { .slave_addr = 9, .reg_start = 0, .reg_count = 10, .poll_interval_ms = 500 };
    rs485_master_add_slave(&s1);

   // rs485_slave_cfg_t s2 = { .slave_addr = 2, .reg_start = 0, .reg_count = 4, .poll_interval_ms = 5000 };
   // rs485_master_add_slave(&s2);

    // Загружаем Wi-Fi настройки из NVS
    ESP_ERROR_CHECK(nvs_load_wifi_settings(&wifi_cfg));

    // Логируем, что получили из памяти
    ESP_LOGI(TAG, "Loaded Wi-Fi settings from NVS:");
    ESP_LOGI(TAG, "Mode: %s", wifi_cfg.mode == WIFI_MODE_STA   ? "STA" : wifi_cfg.mode == WIFI_MODE_AP    ? "AP" : wifi_cfg.mode == WIFI_MODE_APSTA ? "AP+STA" : "UNKNOWN");
    ESP_LOGI(TAG, "STA: SSID=%s  PASS=%s", wifi_cfg.sta_ssid, wifi_cfg.sta_password);
    ESP_LOGI(TAG, "AP : SSID=%s  PASS=%s  Channel=%d", wifi_cfg.ap_ssid, wifi_cfg.ap_password, wifi_cfg.ap_channel);

    // Инициализируем Wi-Fi напрямую из wifi_cfg
    ESP_ERROR_CHECK(wifi_manager_init(&wifi_cfg));


    if (ethernet_init() == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet initialized. Waiting for IP...");
       
    } else {
        ESP_LOGE(TAG, "Ethernet initialization failed!");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    ota_init();
    if (web_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    initialize_sntp();
   
    websocket_client_start(user.serial, user.account_login, user.account_password , user.node_name); 
    xTaskCreate(websocket_reconnect_task, "ws_reconnect_task", 4096, NULL, 5, NULL);
    start_wifi_manager_task();
}


