

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
    // ==========================================================
    // 1) Ищем command_type
    // ==========================================================
    char *cmd_ptr = strstr(json, "\"command_type\"");
    if (!cmd_ptr) return false;

    char command_type[32] = {0};

    char *start = strchr(cmd_ptr, ':');
    if (!start || !(start = strchr(start, '"'))) return true;
    start++;

    char *end = strchr(start, '"');
    if (!end) return true;

    int len = end - start;
    if (len >= sizeof(command_type)) return true;

    memcpy(command_type, start, len);
    command_type[len] = 0;

    //ESP_LOGI(TAG, "⚙️ WS Settings command: %s", command_type);

    // ==========================================================
    // 2) GET SETTINGS
    // ==========================================================
    if (strcmp(command_type, "get_settings") == 0)
    {
        char category[32] = "all";

        char *cat_ptr = strstr(json, "\"settings_requested\"");
        if (cat_ptr)
        {
            char *s = strchr(cat_ptr, ':');
            if (s && (s = strchr(s, '"')))
            {
                s++;
                char *e = strchr(s, '"');
                if (e)
                {
                    int l = e - s;
                    if (l < sizeof(category))
                    {
                        memcpy(category, s, l);
                        category[l] = 0;
                    }
                }
            }
        }

        ESP_LOGI(TAG, "📥 Requested category: %s", category);

        char ответ[768];

        // ==================================================
        // USER SETTINGS
        // ==================================================
        if (strcmp(category, "user") == 0)
        {
            snprintf(ответ, sizeof(ответ),
                     "{"
                     "\"command_type\":\"settings_response\","
                     "\"category\":\"user\","
                     "\"data\":{"
                     "\"login\":\"%s\""
                     "}"
                     "}",
                     user_cfg.login);
        }

        // ==================================================
        // ACCOUNT SETTINGS
        // ==================================================
        else if (strcmp(category, "account") == 0)
        {
            snprintf(ответ, sizeof(ответ),
                     "{"
                     "\"command_type\":\"settings_response\","
                     "\"category\":\"account\","
                     "\"data\":{"
                     "\"account_login\":\"%s\","
                     "\"node_name\":\"%s\""
                     "}"
                     "}",
                     user_cfg.account_login,
                     user_cfg.node_name);
        }

        // ==================================================
        // NETWORK SETTINGS
        // ==================================================
        else if (strcmp(category, "network") == 0)
        {
            snprintf(ответ, sizeof(ответ),
                     "{"
                     "\"command_type\":\"settings_response\","
                     "\"category\":\"network\","
                     "\"data\":{"
                     "\"dhcp_enabled\":%s,"
                     "\"ip\":\"%s\","
                     "\"mask\":\"%s\","
                     "\"gateway\":\"%s\","
                     "\"dns\":\"%s\","
                     "\"port\":%d,"
                     "\"wifi_dhcp_enabled\":%s,"
                     "\"wifi_ip\":\"%s\","
                     "\"wifi_mask\":\"%s\","
                     "\"wifi_gateway\":\"%s\","
                     "\"wifi_dns\":\"%s\""
                     "}"
                     "}",
                     net_cfg.dhcp_enabled ? "true" : "false",
                     net_cfg.ip,
                     net_cfg.mask,
                     net_cfg.gateway,
                     net_cfg.dns,
                     net_cfg.port,
                     net_cfg.wifi_dhcp_enabled ? "true" : "false",
                     net_cfg.wifi_ip,
                     net_cfg.wifi_mask,
                     net_cfg.wifi_gateway,
                     net_cfg.wifi_dns);
                      network_notify_ws();
        }

        // ==================================================
        // WIFI SETTINGS
        // ==================================================
        else if (strcmp(category, "wifi") == 0)
        {
            snprintf(ответ, sizeof(ответ),
                     "{"
                     "\"command_type\":\"settings_response\","
                     "\"category\":\"wifi\","
                     "\"data\":{"
                     "\"mode\":%d,"
                     "\"sta_ssid\":\"%s\","
                     "\"ap_ssid\":\"%s\","
                     "\"ap_channel\":%d"
                     "}"
                     "}",
                     wifi_cfg.mode,
                     wifi_cfg.sta_ssid,
                     wifi_cfg.ap_ssid,
                     wifi_cfg.ap_channel);
        }

        // ==================================================
        // UART SETTINGS
        // ==================================================
        else if (strcmp(category, "uart") == 0)
        {


            int real_data_bits = 8; // по умолчанию
            switch (uart_cfg.data_bits) {
                case UART_DATA_5_BITS: real_data_bits = 5; break;
                case UART_DATA_6_BITS: real_data_bits = 6; break;
                case UART_DATA_7_BITS: real_data_bits = 7; break;
                case UART_DATA_8_BITS: real_data_bits = 8; break;
            }
            
            int real_stop_bits = 1;
            switch (uart_cfg.stop_bits) {
                case UART_STOP_BITS_1:   real_stop_bits = 1; break;
                case UART_STOP_BITS_1_5: real_stop_bits = 15; break; // 1.5 бит
                case UART_STOP_BITS_2:   real_stop_bits = 2; break;
            }

            snprintf(ответ, sizeof(ответ),
                     "{"
                     "\"command_type\":\"settings_response\","
                     "\"category\":\"uart\","
                     "\"data\":{"
                     "\"baud\":%d,"
                     "\"data_bits\":%d,"
                     "\"stop_bits\":%d,"
                     "\"parity\":%d,"
                     "\"rs485_mode\":%d"
                     "}"
                     "}",
                     uart_cfg.baud_rate,
                     real_data_bits,
                     real_stop_bits,  
                     uart_cfg.parity,
                     uart_cfg.rs485_mode);
        }

        // ==================================================
        // SYSTEM SETTINGS 
        // ==================================================
        else if (strcmp(category, "system") == 0)
        {
            snprintf(ответ, sizeof(ответ),
                    "{"
                    "\"command_type\":\"settings_response\","
                    "\"category\":\"system\","
                    "\"data\":{"
                    "\"refresh_interval\":%d,"
                    "\"log_level\":%d,"
                    "\"debug_mode\":%s,"
                    "\"build_number\":%d,"
                    "\"build_date\":\"%s\","
                    "\"ws_server\":\"%s\""
                    "}"
                    "}",
                    sys.refresh_interval,
                    sys.log_level,
                    sys.debug_mode ? "true" : "false",
                    sys.build_number,
                    sys.build_date,
                    sys.ws_server);
        }

      
// ==================================================
// ALL SETTINGS
// ==================================================
else if (strcmp(category, "all") == 0)
{
    ESP_LOGI(TAG, "📦 Sending ALL settings");

    // USER
    snprintf(ответ, sizeof(ответ),
             "{"
             "\"command_type\":\"settings_response\","
             "\"category\":\"user\","
             "\"data\":{"
             "\"login\":\"%s\""
             "}"
             "}",
             user_cfg.login);
    websocket_send_text(ответ);

    // ACCOUNT
    snprintf(ответ, sizeof(ответ),
             "{"
             "\"command_type\":\"settings_response\","
             "\"category\":\"account\","
             "\"data\":{"
             "\"account_login\":\"%s\","
             "\"node_name\":\"%s\""
             "}"
             "}",
             user_cfg.account_login,
             user_cfg.node_name);
    websocket_send_text(ответ);

    // NETWORK
    snprintf(ответ, sizeof(ответ),
             "{"
             "\"command_type\":\"settings_response\","
             "\"category\":\"network\","
             "\"data\":{"
             "\"dhcp_enabled\":%s,"
             "\"ip\":\"%s\","
             "\"mask\":\"%s\","
             "\"gateway\":\"%s\","
             "\"dns\":\"%s\","
             "\"port\":%d,"
             "\"wifi_dhcp_enabled\":%s,"
             "\"wifi_ip\":\"%s\","
             "\"wifi_mask\":\"%s\","
             "\"wifi_gateway\":\"%s\","
             "\"wifi_dns\":\"%s\""
             "}"
             "}",
             net_cfg.dhcp_enabled ? "true" : "false",
             net_cfg.ip,
             net_cfg.mask,
             net_cfg.gateway,
             net_cfg.dns,
             net_cfg.port,
             net_cfg.wifi_dhcp_enabled ? "true" : "false",
             net_cfg.wifi_ip,
             net_cfg.wifi_mask,
             net_cfg.wifi_gateway,
             net_cfg.wifi_dns);
    websocket_send_text(ответ);
    network_notify_ws();

    // WIFI
    snprintf(ответ, sizeof(ответ),
             "{"
             "\"command_type\":\"settings_response\","
             "\"category\":\"wifi\","
             "\"data\":{"
             "\"mode\":%d,"
             "\"sta_ssid\":\"%s\","
             "\"ap_ssid\":\"%s\","
             "\"ap_channel\":%d"
             "}"
             "}",
             wifi_cfg.mode,
             wifi_cfg.sta_ssid,
             wifi_cfg.ap_ssid,
             wifi_cfg.ap_channel);
    websocket_send_text(ответ);

    // UART
    int real_data_bits = 8;
    switch (uart_cfg.data_bits) {
        case UART_DATA_5_BITS: real_data_bits = 5; break;
        case UART_DATA_6_BITS: real_data_bits = 6; break;
        case UART_DATA_7_BITS: real_data_bits = 7; break;
        case UART_DATA_8_BITS: real_data_bits = 8; break;
    }

    int real_stop_bits = 1;
    switch (uart_cfg.stop_bits) {
        case UART_STOP_BITS_1:   real_stop_bits = 1; break;
        case UART_STOP_BITS_1_5: real_stop_bits = 15; break;
        case UART_STOP_BITS_2:   real_stop_bits = 2; break;
    }

    snprintf(ответ, sizeof(ответ),
             "{"
             "\"command_type\":\"settings_response\","
             "\"category\":\"uart\","
             "\"data\":{"
             "\"baud\":%d,"
             "\"data_bits\":%d,"
             "\"stop_bits\":%d,"
             "\"parity\":%d,"
             "\"rs485_mode\":%d"
             "}"
             "}",
             uart_cfg.baud_rate,
             real_data_bits,
             real_stop_bits,
             uart_cfg.parity,
             uart_cfg.rs485_mode);
    websocket_send_text(ответ);

    // SYSTEM
    snprintf(ответ, sizeof(ответ),
             "{"
             "\"command_type\":\"settings_response\","
             "\"category\":\"system\","
             "\"data\":{"
             "\"refresh_interval\":%d,"
             "\"log_level\":%d,"
             "\"debug_mode\":%s,"
             "\"build_number\":%d,"
             "\"build_date\":\"%s\","
             "\"ws_server\":\"%s\""
             "}"
             "}",
             sys.refresh_interval,
             sys.log_level,
             sys.debug_mode ? "true" : "false",
             sys.build_number,
             sys.build_date,
             sys.ws_server);
    websocket_send_text(ответ);
   
    return true;
}

        else
        {
            snprintf(ответ, sizeof(ответ),
                     "{"
                     "\"command_type\":\"settings_response\","
                     "\"category\":\"%s\","
                     "\"error\":\"unknown_category\""
                     "}",
                     category);
        }

        websocket_send_text(ответ);
        return true;
    }

// ==========================================================
// 3) SET SETTINGS
// ==========================================================
    if (strcmp(command_type, "set_settings") == 0)
        {
            ESP_LOGI(TAG, "✍️ SET SETTINGS received");

            char category[32] = {0};

            char *settings_ptr = strstr(json, "\"settings\"");
            if (!settings_ptr)
            {
                websocket_send_text("{\"status\":\"error\",\"message\":\"Missing settings\"}");
                return true;
            }

            if (strstr(settings_ptr, "\"account\""))
                strcpy(category, "account");

            else if (strstr(settings_ptr, "\"wifi\""))
                strcpy(category, "wifi");

            else if (strstr(settings_ptr, "\"network\""))
                strcpy(category, "network");

            else if (strstr(settings_ptr, "\"uart\""))
                strcpy(category, "uart");

            else if (strstr(settings_ptr, "\"user\""))
                strcpy(category, "user");

            else if (strstr(settings_ptr, "\"system\""))
                strcpy(category, "system");

            else if (strstr(settings_ptr, "\"all\""))
                strcpy(category, "all");

            else
            {
                websocket_send_text("{\"status\":\"error\",\"message\":\"Unknown settings category\"}");
                return true;
            }

            ESP_LOGI(TAG, "Category to apply: %s", category);

            // ⚡ Здесь вызываем обработчики
            /*
            if (strcmp(category,"wifi")==0) apply_wifi_settings(...);
            */
 // ==========================================================
// UART SETTINGS
// ==========================================================
if (strcmp(category, "uart") == 0)
{
    ESP_LOGI(TAG, "⚙️ Applying UART settings");

    cJSON *root = cJSON_Parse(json);

    if (!root)
    {
        websocket_send_text(
            "{\"status\":\"error\",\"message\":\"Invalid JSON\"}"
        );
        return true;
    }

    cJSON *settings = cJSON_GetObjectItem(root, "settings");

    if (!settings)
    {
        cJSON_Delete(root);

        websocket_send_text(
            "{\"status\":\"error\",\"message\":\"Missing settings\"}"
        );

        return true;
    }

    // ======================================================
    // UART OBJECT / JSON STRING
    // ======================================================

    cJSON *uart_item = cJSON_GetObjectItem(settings, "uart");

    if (!uart_item)
    {
        cJSON_Delete(root);

        websocket_send_text(
            "{\"status\":\"error\",\"message\":\"Missing uart field\"}"
        );

        return true;
    }

    cJSON *uart_json = NULL;
    bool uart_json_allocated = false;

    // uart = { ... }
    if (cJSON_IsObject(uart_item))
    {
        uart_json = uart_item;
    }
    // uart = "{\"baud\":9600}"
    else if (cJSON_IsString(uart_item))
    {
        uart_json = cJSON_Parse(uart_item->valuestring);

        if (!uart_json)
        {
            cJSON_Delete(root);

            websocket_send_text(
                "{\"status\":\"error\",\"message\":\"Invalid uart JSON string\"}"
            );

            return true;
        }

        uart_json_allocated = true;
    }
    else
    {
        cJSON_Delete(root);

        websocket_send_text(
            "{\"status\":\"error\",\"message\":\"Invalid uart type\"}"
        );

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

    // restart UART
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

    // ======================================================
    // ACK
    // ======================================================

    websocket_send_text("{""\"command_type\":\"set_settings_ack\",""\"status\":\"ok\"," "\"category\":\"uart\"""}" );

    return true;
}

/*
            if (strcmp(category,"network")==0) apply_network_settings(...);
            if (strcmp(category,"account")==0) apply_account_settings(...);
            */
            // ==========================================================
            // SYSTEM COMMAND PARSE
            // ==========================================================
            if (strcmp(category, "system") == 0)
            {
                char value[32] = {0};
                char *sys_ptr = strstr(settings_ptr, "\"system\"");
                if (sys_ptr)
                {
                    char *s = strchr(sys_ptr, ':');
                    if (s && (s = strchr(s, '"')))
                    {
                        s++;
                        char *e = strchr(s, '"');
                        if (e)
                        {
                            int l = e - s;
                            if (l < sizeof(value))
                            {
                                memcpy(value, s, l);
                                value[l] = 0;
                            }
                        }
                    }
                }

                ESP_LOGI(TAG, "🛠 System command: %s", value);

                // ==================================================
                // REBOOT
                // ==================================================
                if (strcmp(value, "reboot") == 0)
                {
                    ESP_LOGW(TAG, "🔄 Reboot command received");
                    websocket_send_text( "{\"command_type\":\"set_settings_ack\",\"status\":\"ok\",\"action\":\"reboot\"}");
                    vTaskDelay(pdMS_TO_TICKS(500)); // дать уйти ack
                    esp_restart();
                }
            }
          //  websocket_send_text("{\"command_type\":\"set_settings_ack\",\"status\":\"ok\"}" );
            return true;
       }
    gpio_link_led(0);
    return true;
}


