


#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_mac.h"

static const char *TAG = "wt32_eth01";
static esp_eth_handle_t eth_handle = NULL;
static esp_netif_t *eth_netif = NULL;

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
    ESP_LOGI(TAG, "Mask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
}

static esp_err_t initialize_ethernet(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet...");
    
    // Initialize network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create Ethernet network interface
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);
    if (eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }

    // Set default netif
    esp_netif_set_default_netif(eth_netif);

    // Ethernet MAC configuration for ESP32 EMAC - NEW API
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    
    // NEW API: Use smi_gpio structure instead of deprecated fields
    emac_config.smi_gpio.mdc_num = 23;    // WT32-ETH01: GPIO23
    emac_config.smi_gpio.mdio_num = 18;   // WT32-ETH01: GPIO18
    
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_OUT_GPIO;  // GPIO17

    // Ethernet PHY configuration
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 0;              // WT32-ETH01: PHY address 0
    phy_config.reset_gpio_num = -1;       // No reset pin

    // MAC configuration
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // Create MAC and PHY instances - CORRECT API
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create MAC");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create PHY");
        return ESP_FAIL;
    }

    // Ethernet configuration
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    
    // Install Ethernet driver
    esp_err_t ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // КЛЮЧЕВОЙ ШАГ: Привязка драйвера к сетевому интерфейсу
    esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Start Ethernet driver
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet initialized successfully");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting WT32-ETH01 Ethernet Bridge...");
    
    if (initialize_ethernet() == ESP_OK) {
        ESP_LOGI(TAG, "Waiting for Ethernet connection and IP address...");
        
        int counter = 0;
        while (1) {
            counter++;
            ESP_LOGI(TAG, "System running... (%d)", counter);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
    } else {
        ESP_LOGE(TAG, "Ethernet initialization failed!");
        
        // Even if init failed, keep running for debugging
        int counter = 0;
        while (1) {
            counter++;
            ESP_LOGW(TAG, "System running but Ethernet failed (%d)", counter);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
    }
}




