

#include "websocket_events.h"
#include "websocket_client.h"
#include "ws_device_settings.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_netif.h"
#include <inttypes.h>

#include "rs485_master.h"
#include "ws_server.h"
#include "gpio_manager.h"
#include "ota_pull.h"
#include "nvs_settings.h"
#include "network_state.h"
#include "modbus_tcp_client.h"

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

extern void ota_task(void *pvParameter);
extern void ws_broadcast(const char *text);

static bool handle_hex_data(const char *json);
static bool handle_pi30_data(const char *json);
static bool handle_mtcp_data(const char *json);
static bool handle_msg_data(const char *json);
static bool handle_ota_update(const char *json, const char *session_id);

static inline uint32_t ticks_to_ms(uint32_t ticks)
{
    return ticks * portTICK_PERIOD_MS;
}

bool websocket_process_message(const char *json)
{   
   // log_heap_full("START");
    gpio_link_led(1);
    if (!json) return false;
    handle_msg_data(json);
   // log_heap_full("after msg");
    handle_hex_data(json);
   // log_heap_full("after hex");
    handle_pi30_data(json);
   // log_heap_full("after pi30");
    handle_mtcp_data(json);
   // log_heap_full("after mtcp");
    handle_settings_command(json);
  //  log_heap_full("after settings");
    handle_ota_update(json, ws_session_id);
   // log_heap_full("after ota");


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
        snprintf(msg, sizeof(msg),"{\"diag\":\"Modbus update : %" PRIu32 "\"}", delta_ms);
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
     gpio_link_led(0);
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
                 "{\"diag\":\"PI30 update : %" PRIu32 "\"}", delta_ms);
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
         gpio_link_led(0);
    return true;
}

static bool handle_mtcp_data(const char *json)
{


  char command_type[32] = {0};

    if (!extract_json_value(json, "command_type", command_type, sizeof(command_type))) {
        return false;
    }

    if (strcmp(command_type, "modbus_tcp") != 0) {
        return false;
    }
 
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "MTCP JSON parse error");
        return false;
    }

    cJSON *type = cJSON_GetObjectItem(root, "command_type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "modbus_tcp") != 0) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *ip         = cJSON_GetObjectItem(root, "ip");
    cJSON *port       = cJSON_GetObjectItem(root, "port");
    cJSON *unit_id    = cJSON_GetObjectItem(root, "unit_id");
    cJSON *func       = cJSON_GetObjectItem(root, "func");
    cJSON *start_addr = cJSON_GetObjectItem(root, "start_addr");
    cJSON *quantity   = cJSON_GetObjectItem(root, "quantity");
    cJSON *value      = cJSON_GetObjectItem(root, "value");

    if (!cJSON_IsString(ip) ||
        !cJSON_IsNumber(port) ||
        !cJSON_IsNumber(unit_id) ||
        !cJSON_IsNumber(func) ||
        !cJSON_IsNumber(start_addr))
    {
        ESP_LOGE(TAG, "MTCP missing fields");
        cJSON_Delete(root);
        return true;
    }

    const char *ip_str = ip->valuestring;

    uint16_t port_v       = port->valueint;
    uint8_t  unit         = unit_id->valueint;
    uint8_t  func_v       = func->valueint;
    uint16_t start        = start_addr->valueint;
    uint16_t qty          = quantity ? quantity->valueint : 0;
    uint16_t val          = value ? value->valueint : 0;

  //  ESP_LOGI(TAG, "📡 MTCP request -> %s:%d unit=%d func=%d start=%d qty=%d val=%d", ip_str, port_v, unit, func_v, start, qty, val);

             if (diagnostics_active) {
    char diag_msg[256];

    snprintf(diag_msg, sizeof(diag_msg),
             "{\"command_type\":\"MTCP request -> %s:%u unit=%u func=%u start=%u qty=%u val=%u\"}",
             ip_str,
             port_v,
             unit,
             func_v,
             start,
             qty,
             val);

   // websocket_send_text(diag_msg);
    ws_broadcast(diag_msg);


}


char cloud_msg[256];

snprintf(cloud_msg, sizeof(cloud_msg),
         "{\"command_type\":\"Modbus TCP request received\","
         "\"ip\":\"%s\","
         "\"port\":%u,"
         "\"unit_id\":%u,"
         "\"func\":%u,"
         "\"start_addr\":%u,"
         "\"quantity\":%u,"
         "\"value\":%u}",
         ip_str,
         port_v,
         unit,
         func_v,
         start,
         qty,
         val);

websocket_send_text(cloud_msg);
    static uint8_t resp[256];
    int resp_len = 0;


    esp_err_t err = modbus_tcp_request(ip_str,port_v,unit,func_v,start,qty,val,resp,&resp_len);

    if (err == ESP_OK) {
   static char hex[520];

    if (resp_len > 0) {
        bytes_to_hex(resp, resp_len, hex, sizeof(hex));
    } else {
        hex[0] = '\0';
    }

   // ESP_LOGI(TAG, "📥 MTCP MODBUS: %s", hex);

    // command_name
    cJSON *cmd_name = cJSON_GetObjectItem(root, "command_name");

    const char *name = (cJSON_IsString(cmd_name)) ? cmd_name->valuestring : "unknown";

    // ---- WS JSON ----
    static char ws_msg[1024];

     snprintf(ws_msg, sizeof(ws_msg),
        "{"
        "\"command_type\":\"modbus_tcp_response\","
        "\"command_name\":\"%s\","
        "\"hex_data\":\"%s\""
        "}",
        name,
        hex
    );

    websocket_send_text(ws_msg);

  //  ESP_LOGI(TAG, "📤 WS sent: %s", ws_msg);
}

    cJSON_Delete(root);
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
 gpio_link_led(0);
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
    xTaskCreatePinnedToCore(ota_task,"ota_task",8192,url_copy,5,NULL,1);

    cJSON_Delete(root);
    gpio_link_led(0);
    return true;
}



