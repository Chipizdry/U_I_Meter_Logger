



#include "ethernet_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "ethernet_manager";

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static bool s_connected = false;

// ======================= Event Handlers =======================

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            s_connected = true;
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            s_connected = false;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            s_connected = false;
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
    const gpio_num_t rst_pin = GPIO_NUM_16;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << rst_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "PHY Reset: DONE");
}

// ======================= Ethernet Init =======================
esp_err_t ethernet_init(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet...");

    phy_hard_reset();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }

#if ETHERNET_USE_DHCP == 0
    // Статическая IP-конфигурация
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_netif_ip_info_t ip_info;
    inet_pton(AF_INET, ETHERNET_STATIC_IP, &ip_info.ip);
    inet_pton(AF_INET, ETHERNET_NETMASK, &ip_info.netmask);
    inet_pton(AF_INET, ETHERNET_GATEWAY, &ip_info.gw);
    esp_netif_set_ip_info(s_eth_netif, &ip_info);
    ESP_LOGI(TAG, "Using static IP: %s", ETHERNET_STATIC_IP);
#else
    ESP_LOGI(TAG, "Using DHCP");
#endif

    // --- Конфигурация MAC и PHY ---
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = 23;
    emac_config.smi_gpio.mdio_num = 18;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 0;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth_handle));
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "Ethernet started successfully");
    return ESP_OK;
}

// ======================= Deinit =======================
esp_err_t ethernet_deinit(void)
{
    if (s_eth_handle) {
        esp_eth_stop(s_eth_handle);
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }

    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    ESP_LOGI(TAG, "Ethernet stopped");
    return ESP_OK;
}

// ======================= Status =======================
bool ethernet_is_connected(void)
{
    return s_connected;
}

esp_netif_ip_info_t ethernet_get_ip_info(void)
{
    esp_netif_ip_info_t info = {0};
    if (s_eth_netif) {
        esp_netif_get_ip_info(s_eth_netif, &info);
    }
    return info;
}
