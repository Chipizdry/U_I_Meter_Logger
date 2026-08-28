

#include "ws_device_settings.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "rs485_master.h"
#include "ws_server.h"
#include "gpio_manager.h"
#include "ota_pull.h"
#include "nvs_settings.h"
#include "network_state.h"
#include "websocket_client.h"
#include "modbus_tcp_client.h"


static const char *TAG = "ws_device_settings";

extern network_settings_t net_cfg;
extern wifi_settings_t wifi_cfg;
extern uart_settings_t uart_cfg;
extern user_settings_t user_cfg;
extern system_settings_t sys;
extern void fill_netif_ip_info(const char *ifkey,char *ip,char *mask,char *gw,char *dns,size_t len);

static void send_account_settings(void);
static void send_user_settings(void);
static void send_network_settings(void);
static void send_wifi_settings(void);
static void send_uart_settings(void);
static void send_system_settings(void);
static void send_ack(const char *category);
static void send_action_ack(const char *action);
static void send_error(const char *msg);

static void ws_send_json(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);

    if (json)
    {
        websocket_send_text(json);
        free(json);
    }
    cJSON_Delete(root);
}


static int uart_data_bits_to_int(uart_word_length_t bits)
{
    switch (bits)
    {
        case UART_DATA_5_BITS: return 5;
        case UART_DATA_6_BITS: return 6;
        case UART_DATA_7_BITS: return 7;
        case UART_DATA_8_BITS: return 8;
        default: return 8;
    }
}

static int uart_stop_bits_to_int(uart_stop_bits_t bits)
{
    switch (bits)
    {
        case UART_STOP_BITS_1:   return 1;
        case UART_STOP_BITS_1_5: return 15;
        case UART_STOP_BITS_2:   return 2;
        default: return 1;
    }
}

static void send_user_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "settings_response");
    cJSON_AddStringToObject(root, "category","user");
    cJSON_AddStringToObject(data, "login", user_cfg.login);
    cJSON_AddItemToObject(root,"data",data);
    ws_send_json(root);
}


static void send_uart_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "settings_response");
    cJSON_AddStringToObject(root, "category", "uart");
    cJSON_AddNumberToObject(data,"baud",uart_cfg.baud_rate);
    cJSON_AddNumberToObject(data,"data_bits", uart_data_bits_to_int(uart_cfg.data_bits));
    cJSON_AddNumberToObject(data, "stop_bits",uart_stop_bits_to_int(uart_cfg.stop_bits));
    cJSON_AddNumberToObject(data,"parity",uart_cfg.parity);
    cJSON_AddBoolToObject(data,"rs485_mode",uart_cfg.rs485_mode);
    cJSON_AddItemToObject(root, "data", data);
    ws_send_json(root);
}

static void send_account_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "settings_response");
    cJSON_AddStringToObject(root, "category", "account");
    cJSON_AddStringToObject(data, "account_login",user_cfg.account_login);
    cJSON_AddStringToObject( data, "node_name", user_cfg.node_name);
    cJSON_AddItemToObject(root, "data", data);
    ws_send_json(root);
}


static void send_network_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "settings_response");
    cJSON_AddStringToObject(root, "category", "network");
    cJSON_AddBoolToObject(data, "dhcp_enabled", net_cfg.dhcp_enabled);
    cJSON_AddStringToObject(data, "ip", net_cfg.ip);
    cJSON_AddStringToObject(data, "mask", net_cfg.mask);
    cJSON_AddStringToObject(data, "gateway", net_cfg.gateway);
    cJSON_AddStringToObject(data, "dns", net_cfg.dns);
    cJSON_AddNumberToObject(data, "port", net_cfg.port);
    cJSON_AddBoolToObject(data, "wifi_dhcp_enabled", net_cfg.wifi_dhcp_enabled);
    cJSON_AddStringToObject(data, "wifi_ip", net_cfg.wifi_ip);
    cJSON_AddStringToObject(data, "wifi_mask", net_cfg.wifi_mask);
    cJSON_AddStringToObject(data, "wifi_gateway", net_cfg.wifi_gateway);
    cJSON_AddStringToObject(data, "wifi_dns", net_cfg.wifi_dns);
    cJSON_AddItemToObject(root, "data", data);
    ws_send_json(root);
    network_notify_ws();
}


static void send_wifi_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "settings_response");
    cJSON_AddStringToObject(root, "category", "wifi");
    cJSON_AddNumberToObject(data, "mode", wifi_cfg.mode);
    cJSON_AddStringToObject(data, "sta_ssid", wifi_cfg.sta_ssid);
    cJSON_AddStringToObject(data, "ap_ssid", wifi_cfg.ap_ssid);
    cJSON_AddNumberToObject(data, "ap_channel", wifi_cfg.ap_channel);
    cJSON_AddItemToObject(root, "data", data);
    ws_send_json(root);
}

static void send_system_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "settings_response");
    cJSON_AddStringToObject(root, "category", "system");
    cJSON_AddNumberToObject(data, "refresh_interval", sys.refresh_interval);
    cJSON_AddNumberToObject(data, "log_level", sys.log_level);
    cJSON_AddBoolToObject(data, "debug_mode", sys.debug_mode);
    cJSON_AddNumberToObject(data, "build_number", sys.build_number);
    cJSON_AddStringToObject(data, "build_date", sys.build_date);
    cJSON_AddStringToObject(data, "ws_server", sys.ws_server);
    cJSON_AddItemToObject(root, "data", data);
    ws_send_json(root);
}

static void send_ack(const char *category)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "set_settings_ack");
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "category", category);
    ws_send_json(root);
}

static void send_action_ack(const char *action)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_type", "set_settings_ack");
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "action", action);
    ws_send_json(root);
}

static void send_error(const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", msg);
    ws_send_json(root);
}

bool handle_settings_command(const char *json)
{
    // Если DHCP включен, берем текущие настройки с Ethernet
  
        if (net_cfg.dhcp_enabled) {
        fill_netif_ip_info("ETH_DEF", net_cfg.ip, net_cfg.mask,net_cfg.gateway,net_cfg.dns, sizeof(net_cfg.ip) );
    }

        if (net_cfg.wifi_dhcp_enabled &&
        (wifi_cfg.mode == WIFI_MODE_STA || wifi_cfg.mode == WIFI_MODE_APSTA)) {
        fill_netif_ip_info("WIFI_STA_DEF", net_cfg.wifi_ip, net_cfg.wifi_mask, net_cfg.wifi_gateway, net_cfg.wifi_dns,sizeof(net_cfg.wifi_ip) );
    }


    cJSON *root = cJSON_Parse(json);

    if (!root)
    {
        send_error("Invalid JSON");
        return false;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command_type");

    if (!cJSON_IsString(cmd))
    {
        cJSON_Delete(root);
        send_error("Missing command_type");
        return false;
    }

    const char *command_type = cmd->valuestring;
    //ESP_LOGI(TAG, "⚙️ WS Settings command: %s", command_type);

    // ==========================================================
    // 2) GET SETTINGS
    // ==========================================================
    if (strcmp(command_type, "get_settings") == 0)
    {

       
      const char *category = "all";

            cJSON *cat = cJSON_GetObjectItem(root, "settings_requested");

            if (cJSON_IsString(cat))
            {
                category = cat->valuestring;
            }

        ESP_LOGI(TAG, "📥 Requested category: %s", category);

        if (strcmp(category, "user") == 0)
            {
                send_user_settings();
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }
        else if (strcmp(category, "account") == 0)
            {
                send_account_settings();
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }

        else if (strcmp(category, "network") == 0)
            {
                send_network_settings();
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }
 
        else if (strcmp(category, "wifi") == 0)
            {
                send_wifi_settings();
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }
                           
        else if (strcmp(category, "uart") == 0)
            {
                send_uart_settings();
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }

       else if (strcmp(category, "system") == 0)
            {
                send_system_settings();
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }


        else if (strcmp(category, "all") == 0)
            {
                ESP_LOGI(TAG, "📦 Sending ALL settings");
                send_user_settings();
                send_account_settings();
                send_network_settings();
                send_wifi_settings();
                send_uart_settings();
                send_system_settings(); 
                cJSON_Delete(root);
                gpio_link_led(0);
                return true;
            }

        else
            {
                send_error("Unknown settings category");
                cJSON_Delete(root);
                return false;
            }
        cJSON_Delete(root);    
        return true;
    }

// ==========================================================
//  SET SETTINGS
// ==========================================================
    if (strcmp(command_type, "set_settings") == 0)
        {
        ESP_LOGI(TAG, "✍️ SET SETTINGS received");
        const char *category = NULL;
        cJSON *settings = cJSON_GetObjectItem(root, "settings");

        if (!cJSON_IsObject(settings))
        {
            send_error("Missing settings");
            cJSON_Delete(root);
            return false;
        }

        if (cJSON_HasObjectItem(settings, "account"))
        {
            category = "account";
        }
        else if (cJSON_HasObjectItem(settings, "wifi"))
        {
            category = "wifi";
        }
        else if (cJSON_HasObjectItem(settings, "network"))
        {
            category = "network";
        }
        else if (cJSON_HasObjectItem(settings, "uart"))
        {
            category = "uart";
        }
        else if (cJSON_HasObjectItem(settings, "user"))
        {
            category = "user";
        }
        else if (cJSON_HasObjectItem(settings, "system"))
        {
            category = "system";
        }

        if (!category)
        {
            send_error("Unknown settings category");
            cJSON_Delete(root);
            return false;
        }

        ESP_LOGI(TAG, "Category to apply: %s", category);

 // ==========================================================
// UART SETTINGS
// ==========================================================
if (strcmp(category, "uart") == 0)
{
    ESP_LOGI(TAG, "⚙️ Applying UART settings");


    if (!settings)
    {
        cJSON_Delete(root);

       // websocket_send_text("{\"status\":\"error\",\"message\":\"Missing settings\"}");
        send_error("Missing settings");
        return true;
    }

    // ======================================================
    // UART OBJECT / JSON STRING
    // ======================================================

    cJSON *uart_item = cJSON_GetObjectItem(settings, "uart");

    if (!uart_item)
    {
        cJSON_Delete(root);

       // websocket_send_text( "{\"status\":\"error\",\"message\":\"Missing uart field\"}");
        send_error("Missing uart field");
        return true;
    }

    cJSON *uart_json = NULL;
    bool uart_json_allocated = false;

    // uart = { ... }
    if (cJSON_IsObject(uart_item))
    {
        uart_json = uart_item;
    }
    
    else if (cJSON_IsString(uart_item))
    {
        uart_json = cJSON_Parse(uart_item->valuestring);

        if (!uart_json)
        {
            cJSON_Delete(root);
            // websocket_send_text("{\"status\":\"error\",\"message\":\"Invalid uart JSON string\"}" );
            send_error("Invalid uart JSON string");
            return true;
        }

        uart_json_allocated = true;
    }
    else
    {
        cJSON_Delete(root);
      //  websocket_send_text("{\"status\":\"error\",\"message\":\"Invalid uart type\"}");
        send_error("Invalid uart type");
        return true;
    }

    // ======================================================
    // fields
    // ======================================================

    cJSON *baud      = cJSON_GetObjectItem(uart_json, "baud");
    cJSON *data_bits = cJSON_GetObjectItem(uart_json, "data_bits");
    cJSON *stop_bits = cJSON_GetObjectItem(uart_json, "stop_bits");
    cJSON *parity    = cJSON_GetObjectItem(uart_json, "parity");
    cJSON *mode      = cJSON_GetObjectItem(uart_json, "mode");

    // baud
    if (baud && cJSON_IsNumber(baud))
    {
        uart_cfg.baud_rate = baud->valueint;
    }

    // data bits
    if (data_bits && cJSON_IsNumber(data_bits))
    {
        switch (data_bits->valueint)
        {
            case 5:
                uart_cfg.data_bits = UART_DATA_5_BITS;
                break;

            case 6:
                uart_cfg.data_bits = UART_DATA_6_BITS;
                break;

            case 7:
                uart_cfg.data_bits = UART_DATA_7_BITS;
                break;

            case 8:
            default:
                uart_cfg.data_bits = UART_DATA_8_BITS;
                break;
        }
    }

    // stop bits
    if (stop_bits && cJSON_IsNumber(stop_bits))
    {
        switch (stop_bits->valueint)
        {
            case 1:
                uart_cfg.stop_bits = UART_STOP_BITS_1;
                break;

            case 2:
                uart_cfg.stop_bits = UART_STOP_BITS_2;
                break;

            case 15:
                uart_cfg.stop_bits = UART_STOP_BITS_1_5;
                break;

            default:
                uart_cfg.stop_bits = UART_STOP_BITS_1;
                break;
        }
    }

    // parity
    if (parity && cJSON_IsNumber(parity))
    {
        switch (parity->valueint)
        {
            case UART_PARITY_DISABLE:
            case UART_PARITY_EVEN:
            case UART_PARITY_ODD:
                uart_cfg.parity = parity->valueint;
                break;

            default:
                uart_cfg.parity = UART_PARITY_DISABLE;
                break;
        }
    }

    // mode
    if (mode && cJSON_IsNumber(mode))
    {
        uart_cfg.rs485_mode = mode->valueint;
    }

    ESP_LOGI(
        TAG,
        "UART applied: baud=%d data=%d stop=%d parity=%d mode=%s",
        uart_cfg.baud_rate,
        uart_cfg.data_bits,
        uart_cfg.stop_bits,
        uart_cfg.parity,
        uart_cfg.rs485_mode ? "RS485" : "RS232"
    );

    // ======================================================
    // SAVE SETTINGS
    // ======================================================

    nvs_save_uart_settings(&uart_cfg);

    rs485_master_deinit();

    vTaskDelay(pdMS_TO_TICKS(100));

    rs485_master_init_from_cfg(&uart_cfg, 2048, 1024);

    // ======================================================
    // FREE MEMORY
    // ======================================================

    if (uart_json_allocated)
    {
        cJSON_Delete(uart_json);
    }

    cJSON_Delete(root);
    send_ack("uart");
    return true;
}

            // ==========================================================
            // SYSTEM COMMAND PARSE
            // ==========================================================
          if (strcmp(category, "system") == 0)
            {
                cJSON *system_item = cJSON_GetObjectItem(settings, "system");

                if (!system_item)
                {
                    send_error("Missing system field");
                    cJSON_Delete(root);
                    return false;
                }

                const char *value = NULL;

                // system: "reboot"
                if (cJSON_IsString(system_item))
                {
                    value = system_item->valuestring;
                }
                // system: { "action":"reboot" }
                else if (cJSON_IsObject(system_item))
                {
                    cJSON *action = cJSON_GetObjectItem(system_item, "action");

                    if (cJSON_IsString(action))
                    {
                        value = action->valuestring;
                    }
                }

                if (!value)
                {
                    send_error("Invalid system command");
                    cJSON_Delete(root);
                    return false;
                }

                ESP_LOGI(TAG, "🛠 System command: %s", value);

                if (strcmp(value, "reboot") == 0)
                {
                    ESP_LOGW(TAG, "🔄 Reboot command received");
                    send_action_ack("reboot");
                    cJSON_Delete(root);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                send_error("Unknown system command");
                cJSON_Delete(root);
                return false;
            }
                
       }
         cJSON_Delete(root);
    gpio_link_led(0);
    return true;
}


