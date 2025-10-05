




#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "driver/gpio.h"

#include "littlefs_manager.h"
#include "rs485_master.h"

static const char *TAG = "wt32_eth01";
static esp_eth_handle_t eth_handle = NULL;

// ======================= Event Handlers =======================
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

// ======================= РУЧНОЙ RESET PHY =======================
static void phy_hard_reset(void)
{
    ESP_LOGI(TAG, "Performing HARD PHY reset...");

    // Настройка GPIO16 как выхода
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 16),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Последовательность reset для LAN8720:
    gpio_set_level(GPIO_NUM_16, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_16, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_16, 1);
   
    //Ждем стабилизации PHY (LAN8720 требует минимум 25ms после reset)
    ESP_LOGI(TAG, "Waiting for PHY to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(50));
}


// ======================= Ethernet Init =======================
static esp_err_t initialize_ethernet(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet...");

    // Вариант 1: Пробуем адрес 1
    ESP_LOGI(TAG, "Trying PHY address 1...");

    phy_hard_reset();

    // --- Инициализация сетевого стека ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    if (!eth_netif) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }
    esp_netif_set_default_netif(eth_netif);

    // --- Конфигурация MAC для WT32-ETH01 ---
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // SMI GPIO
    emac_config.smi_gpio.mdc_num = 23;    // GPIO23
    emac_config.smi_gpio.mdio_num = 18;   // GPIO18

    // Внешний clock на GPIO0
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 0;

    ESP_LOGI(TAG, "MAC Config: MDC=%d, MDIO=%d, CLK_MODE=EXT_IN, CLK_GPIO=%d",
             emac_config.smi_gpio.mdc_num, 
             emac_config.smi_gpio.mdio_num,
             emac_config.clock_config.rmii.clock_gpio);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // --- Конфигурация PHY (адрес 1) ---
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    // Пробуем адрес 1 (WT32-ETH01 обычно адрес 1)
    phy_config.phy_addr = 1;

    // ВАЖНО: Указываем -1 чтобы драйвер НЕ управлял reset
    phy_config.reset_gpio_num = -1;       // Reset уже выполнен вручную
    phy_config.reset_timeout_ms = 2000;   // Увеличиваем таймаут
    phy_config.autonego_timeout_ms = 5000;

    ESP_LOGI(TAG, "PHY Config: ADDR=%ld, RST_GPIO=%ld (manual)", 
             (long)phy_config.phy_addr, (long)phy_config.reset_gpio_num);

    // Создание драйверов
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create MAC");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create PHY");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MAC and PHY instances created successfully");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    // Установка драйвера
    ESP_LOGI(TAG, "Installing Ethernet driver...");
    esp_err_t ret = esp_eth_driver_install(&eth_config, &eth_handle);


    ESP_LOGI(TAG, "Ethernet driver installed successfully!");

    esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));

    // Регистрация обработчиков
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_LOGI(TAG, "Starting Ethernet driver...");
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet initialized successfully");
    return ESP_OK;
}

// ======================= Main =======================
void app_main(void)
{
    ESP_LOGI(TAG, "Starting WT32-ETH01 V2 Ethernet Bridge...");

    // Даем время на стабилизацию питания
    vTaskDelay(pdMS_TO_TICKS(2000));

    if (initialize_ethernet() == ESP_OK) {
        ESP_LOGI(TAG, "Waiting for Ethernet connection and IP address...");

        // Инициализируем LittleFS после успешной инициализации Ethernet
        if (littlefs_init() == ESP_OK) {
            ESP_LOGI(TAG, "LittleFS initialized, creating test file...");
            littlefs_write_file("test.txt", "Hello from LittleFS!\n");
            littlefs_list_files();
            littlefs_read_file("test.txt");
        } else {
            ESP_LOGE(TAG, "LittleFS init failed");
        }

        int counter = 0;


        rs485_master_init(9600, 2048, 1024);

////////////////////////////////////////////////////////////
        rs485_slave_cfg_t s1 = {
            .slave_addr = 1,
            .reg_start = 0,
            .reg_count = 6,         // читаем 6 регистров
            .poll_interval_ms = 2000
        };
        int idx1 = rs485_master_add_slave(&s1);
    
        rs485_slave_cfg_t s2 = {
            .slave_addr = 2,
            .reg_start = 0,
            .reg_count = 4,
            .poll_interval_ms = 5000
        };
        int idx2 = rs485_master_add_slave(&s2);
////////////////////////////////////////////////////

/*
        while (1) {
            counter++;
            ESP_LOGI(TAG, "System running... (%d)", counter);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }

        */
    } else {
        ESP_LOGE(TAG, "Ethernet initialization failed!");
        ESP_LOGI(TAG, "Will retry initialization in 10 seconds...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
}


