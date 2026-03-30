

#include "websocket_events.h"
#include "websocket_client.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_netif.h"

#include "rs485_master.h"
#include "ws_server.h"
#include "gpio_manager.h"
#include "ota_pull.h"
#include "nvs_settings.h"
#include "network_state.h"

static const char *TAG = "ws_events";

static uint32_t last_modbus_tick = 0;
static uint32_t last_pi30_tick   = 0;

extern char ws_session_id[128];
extern char ws_rx_buf[512];
extern char cloud_status_msg[32];
extern bool ws_connected;
extern bool diagnostics_active;

static inline uint32_t ticks_to_ms(uint32_t ticks);
extern int hex_to_bytes(const char *in, uint8_t *out, int max_len);
extern bool websocket_send_text(const char *msg);
extern void websocket_disable_reconnect(void);
extern void websocket_enable_reconnect(void);
extern void fill_netif_ip_info(const char *ifkey,char *ip,char *mask,char *gw,char *dns,size_t len);

extern void ota_task(void *pvParameter);
extern void ws_broadcast(const char *text);

static bool handle_hex_data(const char *json);
static bool handle_pi30_data(const char *json);
static bool handle_msg_data(const char *json);
static bool handle_settings_command(const char *json);
static bool handle_ota_update(const char *json, const char *session_id);

static inline uint32_t ticks_to_ms(uint32_t ticks)
{
    return ticks * portTICK_PERIOD_MS;
}

bool websocket_process_message(const char *json)
{
    if (!json) return false;
    handle_msg_data(json);
    handle_hex_data(json);
    handle_pi30_data(json);
    handle_settings_command(json);
    handle_ota_update(json, ws_session_id);

    return true;
}


char *extract_json_value(const char *json, const char *key, char *out, int out_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    char *ptr = strstr(json, pattern);
    if (!ptr) return NULL;

    // ищем ':' после ключа
    char *colon = strchr(ptr, ':');
    if (!colon) return NULL;

    // ищем первую кавычку после ':'
    char *start = strchr(colon, '"');
    if (!start) return NULL;
    start++; // начало значения

    char *end = strchr(start, '"');
    if (!end || end <= start) return NULL;

    int len = end - start;
    if (len >= out_len) len = out_len - 1;

    memcpy(out, start, len);
    out[len] = 0;
    return out;
}


static bool handle_hex_data(const char *json)
{
    char command_type[32] = {0};
    char command_name[32] = {0};
    extract_json_value(json, "command_type", command_type, sizeof(command_type));
    extract_json_value(json, "command_name", command_name, sizeof(command_name));
   
    char *hex_ptr = strstr(json, "\"hex_data\"");
    if (!hex_ptr) return false;

    char *start = strchr(hex_ptr, ':');
    if (!start || !(start = strchr(start, '"'))) {
        ESP_LOGE(TAG, "hex_data: format error");
        return true;
    }
    start++;

    char *end = strchr(start, '"');
    if (!end || end <= start) {
        ESP_LOGE(TAG, "hex_data: empty value");
        return true;
    }

    char hex_str[128] = {0};
    int len = end - start;
    if (len >= sizeof(hex_str)) {
        ESP_LOGE(TAG, "hex_data too long");
        return true;
    }

    memcpy(hex_str, start, len);
   // ESP_LOGI(TAG, "✅ HEX extracted: %s", hex_str);

    // убираем пробелы
    char clean_hex[128] = {0};
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (hex_str[i] != ' ') {
            clean_hex[j++] = hex_str[i];
        }
    }

    // диагностика времени
    uint32_t now = xTaskGetTickCount();
    uint32_t delta_ms = last_modbus_tick ? ticks_to_ms(now - last_modbus_tick) : 0;
    last_modbus_tick = now;

    if (diagnostics_active && delta_ms > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),"{\"diag\":\"Modbus update : %lu\"}", delta_ms);
        ws_broadcast(msg);
    }

    uint8_t bytes[64];
    int byte_len = hex_to_bytes(clean_hex, bytes, sizeof(bytes));
    if (byte_len <= 0) {
        ESP_LOGE(TAG, "HEX parse error");
        return true;
    }

   // ESP_LOGI(TAG, "📤 RS485 send %d bytes", byte_len);
   // ESP_LOG_BUFFER_HEX(TAG, bytes, byte_len);
    rs485_req_t req = {0};
    memcpy(req.data, bytes, byte_len);
    req.len = byte_len;
    strncpy(req.cmd, command_type[0] ? command_type : "UNKNOWN", sizeof(req.cmd) - 1);
    strncpy(req.command_name, command_name[0] ? command_name : "UNKNOWN", sizeof(req.command_name) - 1);
    rs485_master_send_req(&req);
    return true;
 }



static bool handle_pi30_data(const char *json)
{
    char *pi30_ptr = strstr(json, "\"pi30\"");
    if (!pi30_ptr) {
        return false;
    }

    char *start = strchr(pi30_ptr, ':');
    if (!start || !(start = strchr(start, '"'))) {
        ESP_LOGE(TAG, "pi30: format error");
        return true;
    }
    start++;

    char *end = strchr(start, '"');
    if (!end || end <= start) {
        ESP_LOGE(TAG, "pi30: empty value");
        return true;
    }

    char hex_str[128] = {0};
    int len = end - start;
    if (len >= sizeof(hex_str)) {
        ESP_LOGE(TAG, "pi30 too long");
        return true;
    }

    memcpy(hex_str, start, len);
    //ESP_LOGI(TAG, "🔶 PI30 HEX: %s", hex_str);

    char clean_hex[128] = {0};
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (hex_str[i] != ' ' && hex_str[i] != '\n') {
            clean_hex[j++] = hex_str[i];
        }
    }

    uint32_t now = xTaskGetTickCount();
    uint32_t delta_ms = last_pi30_tick ? ticks_to_ms(now - last_pi30_tick) : 0;
    last_pi30_tick = now;

    if (diagnostics_active && delta_ms > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "{\"diag\":\"PI30 update : %lu\"}", delta_ms);
        ws_broadcast(msg);
    }

    uint8_t bytes[64];
    int byte_len = hex_to_bytes(clean_hex, bytes, sizeof(bytes));
    if (byte_len <= 0) {
        ESP_LOGE(TAG, "PI30 HEX parse error");
        return true;
    }

    // ASCII лог
    char ascii[128];
    int ai = 0;
    for (int i = 0; i < byte_len; i++) {
        char c = bytes[i];
        ascii[ai++] = (c >= 32 && c < 127) ? c : '.';
    }
    ascii[ai] = 0;

    ESP_LOGI(TAG, "🔤 PI30 ASCII: %s", ascii);
  //  ESP_LOGI(TAG, "📤 UART send %d bytes", byte_len);
  //  ESP_LOG_BUFFER_HEX(TAG, bytes, byte_len);

        rs485_req_t req = {0};
        memcpy(req.data, bytes, byte_len);
        req.len = byte_len;
        // 🔥 извлекаем команду ИЗ ЗАПРОСА
        if (byte_len >= 5 &&
            bytes[0] >= 'A' && bytes[0] <= 'Z') {
            // QPIGS / QMOD / QFLAG / etc
            memcpy(req.cmd, bytes, 5);
            req.cmd[5] = 0;
        } else {
            strcpy(req.cmd, "UNKNOWN");
        }
        rs485_master_send_req(&req);
    return true;
}




 static bool handle_msg_data(const char *json) {
    char *msg_ptr = strstr(json, "\"cloud_status\"");
    if (!msg_ptr) {
        return false;
    }   
  // сохранить статус для get_settings ----
    char *ptr = strstr(ws_rx_buf, "\"cloud_status\"");
    if (ptr) {
        char *start = strchr(ptr, ':');
        if (start && (start = strchr(start, '"'))) {
            start++;
            char *end = strchr(start, '"');
            if (end && end > start) {
                size_t len = end - start;
                if (len < sizeof(cloud_status_msg)) {
                    memcpy(cloud_status_msg, start, len);
                    cloud_status_msg[len] = 0;
                    ESP_LOGI(TAG, "💾 Saved cloud_status_msg = %s", cloud_status_msg);
                    ESP_LOGI(TAG, "📡 Broadcasting cloud status to local WS clients: %s", ws_rx_buf);
                    ws_broadcast(ws_rx_buf);

                    if (strcmp(cloud_status_msg, "authenticated") == 0 ||
                            strcmp(cloud_status_msg, "connected") == 0) {
                                 gpio_set_net_led(true);
                            ws_connected = true;

                             char time_str[64];
                            get_time_iso(time_str, sizeof(time_str));

                            char msg[128];
                            snprintf(msg, sizeof(msg),
                                "{\"command_type\":\"device_online\",\"time\":\"%s\"}",
                                time_str);

                            websocket_send_text(msg);
                        } else {
                            ws_connected = false;
                        }
                
                }
            }
        }
    }
 return true;

}   



static bool handle_settings_command(const char *json)
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
        // SYSTEM SETTINGS (пример)
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
            if (strcmp(category,"uart")==0) apply_uart_settings(...);
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

    return true;
}



static bool handle_ota_update(const char *json, const char *session_id)
{
    if (!strstr(json, "update_firmware"))
        return false;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON");
        return true;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "firmware_url");
    if (!cJSON_IsString(url_item) || !url_item->valuestring) {
        ESP_LOGE(TAG, "firmware_url not found");
        cJSON_Delete(root);
        return true;
    }

    const char *firmware_url = url_item->valuestring;

    ESP_LOGW(TAG, "🚀 OTA pull from: %s", firmware_url);

    // Отключаем автоподключение WS на время OTA
    websocket_disable_reconnect();

    // 🔥 ВАЖНО: используем вашу pull-логику
  //  esp_err_t ret = ota_pull_start(firmware_url);
    char *url_copy = strdup(firmware_url);
    xTaskCreatePinnedToCore(
    ota_task,
    "ota_task",
    8192,
    url_copy,
    5,
    NULL,
    1
);

   
    cJSON_Delete(root);
    return true;
}



