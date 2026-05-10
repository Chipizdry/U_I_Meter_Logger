

#include "ethernet_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "driver/gpio.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/ip.h"
#include "lwip/raw.h"
#include "lwip/netif.h"

#include "nvs_settings.h"
#include "network_state.h"
#include "gpio_manager.h"

#include "esp_eth_mac.h"
#include "esp_netif_net_stack.h"

#include "esp_timer.h"
#include <math.h>

static const char *TAG = "ethernet_manager";

extern esp_netif_t *eth_netif;

int g_eth_speed_mbps = 0;

float g_eth_rx_kbps = 0;
float g_eth_tx_kbps = 0;

static uint64_t g_tx_bytes = 0;
static uint64_t g_rx_bytes = 0;

static uint64_t last_rx = 0;
static uint64_t last_tx = 0;

static int64_t last_time = 0;

#define ETH_SMOOTH_ALPHA 0.2f
#define ETH_NOISE_KBPS   1.0f

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;

static bool s_connected = false;

static void phy_hard_reset(void);
static void update_eth_traffic(void);
static void ethernet_traffic_task(void *pvParameters);

void ethernet_start_traffic_monitoring(void);

static esp_err_t eth_input_hook(esp_eth_handle_t hdl,uint8_t *buffer,uint32_t length,void *priv);
static err_t eth_tx_hook(struct netif *netif, struct pbuf *p);
static err_t (*s_orig_linkoutput)( struct netif *netif, struct pbuf *p) = NULL;

// =====================================================
// RX HOOK
// =====================================================

static esp_err_t eth_input_hook(esp_eth_handle_t hdl,uint8_t *buffer, uint32_t length,void *priv)
{
    if (length > 0 && length < 1600) {
        g_rx_bytes += length;
    }

    return esp_netif_receive(
        (esp_netif_t *)priv,
        buffer,
        length,
        NULL);
}


// =====================================================
// TX HOOK
// =====================================================

static err_t eth_tx_hook(struct netif *netif, struct pbuf *p)
{
    if (p && p->tot_len > 0) {
        g_tx_bytes += p->tot_len;
        ESP_LOGI(TAG, "TX frame: %d", p->tot_len);
    }

    return s_orig_linkoutput(netif, p);
}


// =====================================================
// PRINT TRAFFIC
// =====================================================

void print_traffic(void)
{
    printf("RX bytes: %llu\n", g_rx_bytes);
    printf("TX bytes: %llu\n", g_tx_bytes);
}


// =====================================================
// LINK SPEED
// =====================================================

static void update_eth_link_speed(void)
{
    if (!s_eth_handle)
        return;

    eth_speed_t speed;

    if (esp_eth_ioctl(
            s_eth_handle,
            ETH_CMD_G_SPEED,
            &speed) == ESP_OK)
    {
        switch (speed) {

        case ETH_SPEED_10M:
            g_eth_speed_mbps = 10;
            break;

        case ETH_SPEED_100M:
            g_eth_speed_mbps = 100;
            break;

        default:
            g_eth_speed_mbps = 0;
            break;
        }
    }
}


// =====================================================
// TRAFFIC TASK
// =====================================================

static void ethernet_traffic_task(void *pvParameters)
{
    while (1) {

        update_eth_traffic();

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}


// =====================================================
// UPDATE TRAFFIC
// =====================================================

static void update_eth_traffic(void)
{

    int64_t now = esp_timer_get_time() / 1000;

    if (last_time == 0) {

        last_rx = g_rx_bytes;
        last_tx = g_tx_bytes;
        last_time = now;

        return;
    }

    int64_t dt = now - last_time;

    if (dt < 1000)
        return;

    uint64_t rx_diff = g_rx_bytes - last_rx;
    uint64_t tx_diff = g_tx_bytes - last_tx;

    float sec = dt / 1000.0f;

    float rx_kbps =
        (rx_diff * 8.0f) / sec / 1000.0f;

    float tx_kbps =
        (tx_diff * 8.0f) / sec / 1000.0f;

    // шумовой фильтр

    if (rx_kbps < ETH_NOISE_KBPS)
        rx_kbps = 0;

    if (tx_kbps < ETH_NOISE_KBPS)
        tx_kbps = 0;

    // EMA smoothing

    g_eth_rx_kbps =
        g_eth_rx_kbps * (1.0f - ETH_SMOOTH_ALPHA)
        + rx_kbps * ETH_SMOOTH_ALPHA;

    g_eth_tx_kbps =
        g_eth_tx_kbps * (1.0f - ETH_SMOOTH_ALPHA)
        + tx_kbps * ETH_SMOOTH_ALPHA;

    last_rx = g_rx_bytes;
    last_tx = g_tx_bytes;

    last_time = now;

    ESP_LOGI(TAG,
             "ETH: RX=%.2f kbps TX=%.2f kbps",
             g_eth_rx_kbps,
             g_eth_tx_kbps);

    network_notify_ws();
}


// =====================================================
// EVENT HANDLERS
// =====================================================

static void eth_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    switch (event_id) {

    case ETHERNET_EVENT_CONNECTED:

        ESP_LOGI(TAG, "Ethernet Link Up");

        s_connected = true;

        update_eth_link_speed();

        break;

    case ETHERNET_EVENT_DISCONNECTED:

        ESP_LOGI(TAG, "Ethernet Link Down");
        s_connected = false;
        network_set_ethernet_state(NET_STATE_ETHERNET_DOWN);

        break;

    case ETHERNET_EVENT_START:

        ESP_LOGI(TAG, "Ethernet Started");
        network_set_ethernet_state( NET_STATE_ETHERNET_CONNECTING);

        break;

    case ETHERNET_EVENT_STOP:

        ESP_LOGI(TAG, "Ethernet Stopped");
        network_set_ethernet_state( NET_STATE_ETHERNET_DOWN);
        s_connected = false;

        break;

    default:
        break;
    }
}


static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    update_eth_link_speed();
    network_set_ethernet_state( NET_STATE_ETHERNET_UP);
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
}


// =====================================================
// PHY RESET
// =====================================================

static void phy_hard_reset(void)
{
    gpio_set_level(ETH_RST_PIN, 1);

    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(ETH_RST_PIN, 0);

    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_level(ETH_RST_PIN, 1);

    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "PHY Reset: DONE");
}


// =====================================================
// ETHERNET INIT
// =====================================================

esp_err_t ethernet_init(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet...");

    phy_hard_reset();

    esp_netif_config_t cfg =
        ESP_NETIF_DEFAULT_ETH();

    s_eth_netif = esp_netif_new(&cfg);

    if (!s_eth_netif) {

        ESP_LOGE(TAG,
                 "Failed to create Ethernet netif");

        return ESP_FAIL;
    }

    if (net_cfg.dhcp_enabled) {

        ESP_LOGI(TAG, "Using DHCP mode");

    } else {

        ESP_LOGI(TAG,
                 "Using static IP: %s",
                 net_cfg.ip);

        esp_netif_dhcpc_stop(s_eth_netif);

        esp_netif_ip_info_t ip_info;

        inet_pton(AF_INET,
                  net_cfg.ip,
                  &ip_info.ip);

        inet_pton(AF_INET,
                  net_cfg.mask,
                  &ip_info.netmask);

        inet_pton(AF_INET,
                  net_cfg.gateway,
                  &ip_info.gw);

        esp_netif_set_ip_info(
            s_eth_netif,
            &ip_info);
    }

    eth_esp32_emac_config_t emac_config =
        ETH_ESP32_EMAC_DEFAULT_CONFIG();

    emac_config.smi_gpio.mdc_num = 23;
    emac_config.smi_gpio.mdio_num = 18;

    emac_config.clock_config.rmii.clock_mode =
        EMAC_CLK_EXT_IN;

    emac_config.clock_config.rmii.clock_gpio = 0;

    eth_mac_config_t mac_config =
        ETH_MAC_DEFAULT_CONFIG();

    eth_phy_config_t phy_config =
        ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac =
        esp_eth_mac_new_esp32(
            &emac_config,
            &mac_config);

    esp_eth_phy_t *phy =
        esp_eth_phy_new_lan87xx(
            &phy_config);

    esp_eth_config_t eth_config =
        ETH_DEFAULT_CONFIG(mac, phy);

    esp_err_t err =
        esp_eth_driver_install(
            &eth_config,
            &s_eth_handle);

    if (err != ESP_OK) {

        ESP_LOGE(TAG,
                 "Ethernet install failed: %s",
                 esp_err_to_name(err));

        return err;
    }

    esp_netif_attach( s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            ETH_EVENT,
            ESP_EVENT_ANY_ID,
            &eth_event_handler,
            NULL));

    ESP_ERROR_CHECK(esp_event_handler_register( IP_EVENT,IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // RX hook

    ESP_ERROR_CHECK(
        esp_eth_update_input_path(
            s_eth_handle,
            eth_input_hook,
            s_eth_netif));

    // TX hook

    struct netif *lwip_netif =
        esp_netif_get_netif_impl(
            s_eth_netif);

    if (lwip_netif) {

        s_orig_linkoutput =lwip_netif->linkoutput;

       lwip_netif->linkoutput = eth_tx_hook;
    }

    ESP_ERROR_CHECK( esp_eth_start(s_eth_handle));
 
    ethernet_start_traffic_monitoring();
    ESP_LOGI(TAG, "Ethernet started successfully");

    return ESP_OK;
}


// =====================================================
// DEINIT
// =====================================================

esp_err_t ethernet_deinit(void)
{
    if (s_eth_handle) {

        esp_eth_stop(s_eth_handle);

        esp_eth_driver_uninstall(
            s_eth_handle);

        s_eth_handle = NULL;
    }

    if (s_eth_netif) {

        esp_netif_destroy(s_eth_netif);

        s_eth_netif = NULL;
    }

    ESP_LOGI(TAG, "Ethernet stopped");

    return ESP_OK;
}


// =====================================================
// STATUS
// =====================================================

bool ethernet_is_connected(void)
{
    return s_connected;
}


esp_netif_ip_info_t ethernet_get_ip_info(void)
{
    esp_netif_ip_info_t info = {0};

    if (s_eth_netif) {

        esp_netif_get_ip_info(
            s_eth_netif,
            &info);
    }

    return info;
}


// =====================================================
// START MONITORING
// =====================================================

void ethernet_start_traffic_monitoring(void)
{
    xTaskCreate(
        ethernet_traffic_task,
        "eth_traffic",
        4096,
        NULL,
        5,
        NULL);

    ESP_LOGI(TAG,
             "Ethernet traffic monitoring started");
}

