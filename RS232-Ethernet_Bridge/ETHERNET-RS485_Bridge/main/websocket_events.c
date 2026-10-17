

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
#include <math.h>

#include "rs485_master.h"
#include "ws_server.h"
#include "gpio_manager.h"
#include "ota_pull.h"
#include "nvs_settings.h"
#include "network_state.h"
#include "modbus_tcp_client.h"
#include "traceroute.h"

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

typedef enum
{
    WS_MSG_UNKNOWN = 0,
    WS_MSG_CLOUD_STATUS,
    WS_MSG_HEX,
    WS_MSG_PI30,
    WS_MSG_MODBUS_TCP,
    WS_MSG_SETTINGS,
    WS_MSG_OTA,
    WS_MSG_TRACEROUTE

} ws_message_type_t;


static ws_message_type_t detect_message_type(cJSON *root);

static bool handle_hex_data(cJSON *root);
static bool handle_pi30_data(cJSON *root);
static bool handle_mtcp_data(cJSON *root);
static bool handle_msg_data(cJSON *root);
static bool handle_traceroute(cJSON *root);
static bool handle_ota_update(cJSON *root, const char *session_id);


static inline uint32_t ticks_to_ms(uint32_t ticks)
{
    return ticks * portTICK_PERIOD_MS;
}


static ws_message_type_t detect_message_type(cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) {
        return WS_MSG_UNKNOWN;
    }

    cJSON *command_type = cJSON_GetObjectItemCaseSensitive(root, "command_type");

    if (cJSON_IsString(command_type) &&
        command_type->valuestring)
    {
        const char *type = command_type->valuestring;

        if (strcmp(type, "modbus_tcp") == 0) {
            return WS_MSG_MODBUS_TCP;
        }

        if (strcmp(type, "get_settings") == 0 ||
            strcmp(type, "set_settings") == 0)
        {
            return WS_MSG_SETTINGS;
        }

        if (strcmp(type, "update_firmware") == 0) {
            return WS_MSG_OTA;
        }
        if (strcmp(type, "traceroute") == 0) {
        return WS_MSG_TRACEROUTE;
        }
    }

    cJSON *command =
        cJSON_GetObjectItemCaseSensitive(root, "command");

    if (cJSON_IsString(command) &&
        command->valuestring)
    {
        const char *cmd = command->valuestring;

        if (strcmp(cmd, "update_firmware") == 0) {
            return WS_MSG_OTA;
        }
    }

    if (cJSON_HasObjectItem(root, "cloud_status")) {
        return WS_MSG_CLOUD_STATUS;
    }

    if (cJSON_HasObjectItem(root, "pi30")) {
        return WS_MSG_PI30;
    }

    if (cJSON_HasObjectItem(root, "hex_data")) {
        return WS_MSG_HEX;
    }

    return WS_MSG_UNKNOWN;
}

static void traceroute_ws_callback(
    const traceroute_result_t *result,
    void *user_ctx)
{
    (void)user_ctx;

    if (!result)
    {
        return;
    }

    ESP_LOGI( TAG, "Building traceroute report: %s [%s]",result->host,result->destination_ip);

    cJSON *root =
        cJSON_CreateObject();

    if (!root)
    {
        ESP_LOGE( TAG,"Failed to create traceroute JSON");
        return;
    }


    /* =====================================================
     * Header
     * ===================================================== */

    cJSON_AddStringToObject(
        root,
        "command_type",
        "traceroute_result"
    );

    cJSON_AddStringToObject(
        root,
        "host",
        result->host
    );

    cJSON_AddStringToObject(
        root,
        "destination",
        result->destination_ip
    );

    cJSON_AddNumberToObject(
        root,
        "max_hops",
        result->max_hops
    );

    cJSON_AddNumberToObject(
        root,
        "hop_count",
        result->hop_count
    );

    cJSON_AddBoolToObject(
        root,
        "reached",
        result->reached
    );

    cJSON_AddNumberToObject(
        root,
        "error",
        result->error
    );

    cJSON_AddStringToObject(
        root,
        "error_name",
        esp_err_to_name(result->error)
    );


    /* =====================================================
     * Hops
     * ===================================================== */

    cJSON *hops =
        cJSON_AddArrayToObject(
            root,
            "hops"
        );


    for (
        int i = 0;
        i < result->hop_count;
        i++
    )
    {
        const traceroute_hop_t *hop =
            &result->hops[i];


        cJSON *hop_json =
            cJSON_CreateObject();


        if (!hop_json)
        {
            continue;
        }


        cJSON_AddNumberToObject(
            hop_json,
            "hop",
            hop->hop
        );


        if (hop->ip[0] != '\0')
        {
            cJSON_AddStringToObject(
                hop_json,
                "ip",
                hop->ip
            );
        }
        else
        {
            cJSON_AddStringToObject(
                hop_json,
                "ip",
                "*"
            );
        }


        cJSON_AddBoolToObject(
            hop_json,
            "timeout",
            hop->timeout
        );


        cJSON_AddBoolToObject(
            hop_json,
            "destination",
            hop->destination
        );


        cJSON_AddNumberToObject(
            hop_json,
            "responses",
            hop->responses
        );

        /* =============================================
         * RTT array
         * ============================================= */

        cJSON *rtt =
            cJSON_AddArrayToObject(
                hop_json,
                "rtt_ms"
            );


        for (
            int probe = 0;
            probe < TRACEROUTE_PROBES_PER_HOP;
            probe++
        )
        {
            if (hop->rtt_ms[probe] >= 0.0f)
            {
                /*
                 * cJSON печатает float через "%1.15g",
                 * поэтому 8.72f выводится как 8.72000026702881.
                 *
                 * Форматируем сами через snprintf("%.2f")
                 * и подставляем как raw-число — получается ровно
                 * "8.72", парсится любой JSON-библиотекой.
                 */
                char rtt_str[16];

                snprintf(
                    rtt_str,
                    sizeof(rtt_str),
                    "%.2f",
                    (double)hop->rtt_ms[probe]
                );

                cJSON_AddItemToArray(
                    rtt,
                    cJSON_CreateRaw(rtt_str)
                );
            }
            else
            {
                cJSON_AddItemToArray(
                    rtt,
                    cJSON_CreateNull()
                );
            }
        }


        cJSON_AddItemToArray( hops, hop_json);
    }


    /* =====================================================
     * Serialize
     * ===================================================== */

    char *json =
        cJSON_PrintUnformatted(root);


    cJSON_Delete(root);


    if (!json)
    {
        ESP_LOGE(TAG,"Failed to serialize traceroute report");
        return;
    }

    ESP_LOGI( TAG,"Traceroute report size: %d bytes",(int)strlen(json));
    ESP_LOGI(TAG,"Traceroute report: %s",json);

    /* =====================================================
     * ONE WebSocket message
     * ===================================================== */

    websocket_send_text(json);


    free(json);
}


bool websocket_process_message(const char *json)
{
    if (!json) {
        return false;
    }

    gpio_link_led(1);

    cJSON *root = cJSON_Parse(json);

    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON");
        gpio_link_led(0);
        return false;
    }

    ws_message_type_t type = detect_message_type(root);
    bool handled = false;
    const char *command_name = "<none>";
    cJSON *command_type_item =
        cJSON_GetObjectItemCaseSensitive( root, "command_type" );

    if (cJSON_IsString(command_type_item) &&
        command_type_item->valuestring)
    {
        command_name = command_type_item->valuestring;
    }
    else
    {
        cJSON *command_item = cJSON_GetObjectItemCaseSensitive( root, "command" );

        if (cJSON_IsString(command_item) && command_item->valuestring)
        {
            command_name =command_item->valuestring;
        }
    }

    ESP_LOGI( TAG,"WS message: command_type=%s", command_name);

    switch (type)
    {
        case WS_MSG_CLOUD_STATUS:

            handled =
                handle_msg_data(root);

            break;

        case WS_MSG_HEX:

            handled =
                handle_hex_data(root);

            break;

        case WS_MSG_PI30:

            handled =
                handle_pi30_data(root);

            break;

        case WS_MSG_MODBUS_TCP:

            handled =
                handle_mtcp_data(root);

            break;

        case WS_MSG_SETTINGS:

            handled =handle_settings_command(root);
            break;
        case WS_MSG_TRACEROUTE:
            handled = handle_traceroute(root);
            break;

        case WS_MSG_OTA:

            handled = handle_ota_update(root, ws_session_id);
            break;
        



        case WS_MSG_UNKNOWN:
        default:

            ESP_LOGW( TAG, "Unknown WS message type");
            char *debug_json = cJSON_PrintUnformatted(root);
            if (debug_json)
            {
                ESP_LOGW(TAG,"Unknown JSON: %s", debug_json);
                free(debug_json);
            }

            handled = false;
            break;
    }

    cJSON_Delete(root);
    gpio_link_led(0);
    return handled;
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



static bool handle_hex_data(cJSON *root)
{
    cJSON *hex_item = cJSON_GetObjectItem(root, "hex_data");

    if (!cJSON_IsString(hex_item) ||
        !hex_item->valuestring)
    {
        ESP_LOGE(TAG, "hex_data: missing or invalid");
        return true;
    }

    cJSON *command_type_item = cJSON_GetObjectItem(root, "command_type");
    cJSON *command_name_item = cJSON_GetObjectItem(root, "command_name");

    const char *command_type =
        cJSON_IsString(command_type_item)
            ? command_type_item->valuestring
            : "UNKNOWN";

    const char *command_name =
        cJSON_IsString(command_name_item)
            ? command_name_item->valuestring
            : "UNKNOWN";

    const char *hex_str = hex_item->valuestring;

    char clean_hex[128] = {0};

    int j = 0;

    for (int i = 0; hex_str[i] != '\0';i++)
    {
        char c = hex_str[i];

        if (c == ' ' ||
            c == '\n' ||
            c == '\r' ||
            c == '\t')
        {
            continue;
        }

        if (j >= sizeof(clean_hex) - 1)
        {
            ESP_LOGE(TAG, "hex_data too long");
            return true;
        }

        clean_hex[j++] = c;
    }

    clean_hex[j] = '\0';

    /*
     * Diagnostics
     */

    uint32_t now = xTaskGetTickCount();

    uint32_t delta_ms =
        last_modbus_tick
            ? ticks_to_ms(now - last_modbus_tick)
            : 0;

    last_modbus_tick = now;

    if (diagnostics_active && delta_ms > 0)
    {
        char msg[64];
        snprintf( msg, sizeof(msg), "{\"diag\":\"Modbus update : %" PRIu32 "\"}",delta_ms);
        ws_broadcast(msg);
    }

    /*
     * HEX -> bytes
     */

    uint8_t bytes[64];

    int byte_len = hex_to_bytes( clean_hex, bytes,sizeof(bytes));

    if (byte_len <= 0)
    {
        ESP_LOGE(TAG, "HEX parse error");
        return true;
    }

    /*
     * RS485 request
     */

    rs485_req_t req = {0};

    memcpy(req.data, bytes, byte_len);

    req.len = byte_len;

    strncpy(
        req.cmd,
        command_type,
        sizeof(req.cmd) - 1
    );

    req.cmd[sizeof(req.cmd) - 1] = '\0';

    strncpy(
        req.command_name,
        command_name,
        sizeof(req.command_name) - 1
    );

    req.command_name[sizeof(req.command_name) - 1] = '\0';

    rs485_master_send_req(&req);

    return true;
}

static bool handle_pi30_data(cJSON *root)
{
    cJSON *pi30_item =
        cJSON_GetObjectItem(root, "pi30");

    if (!cJSON_IsString(pi30_item) ||
        !pi30_item->valuestring)
    {
        ESP_LOGE(TAG, "pi30: invalid value");
        return true;
    }

    const char *hex_str = pi30_item->valuestring;

    char clean_hex[128] = {0};

    int j = 0;

    for (int i = 0;
         hex_str[i] != '\0';
         i++)
    {
        char c = hex_str[i];

        if (c == ' ' ||
            c == '\n' ||
            c == '\r' ||
            c == '\t')
        {
            continue;
        }

        if (j >= sizeof(clean_hex) - 1)
        {
            ESP_LOGE(TAG, "pi30 cleaned HEX too long");
            return true;
        }

        clean_hex[j++] = c;
    }

    clean_hex[j] = '\0';

    /*
     * Diagnostics
     */

    uint32_t now = xTaskGetTickCount();

    uint32_t delta_ms =
        last_pi30_tick
            ? ticks_to_ms(now - last_pi30_tick)
            : 0;

    last_pi30_tick = now;

    if (diagnostics_active && delta_ms > 0)
    {
        char msg[64];

        snprintf(
            msg,
            sizeof(msg),
            "{\"diag\":\"PI30 update : %" PRIu32 "\"}",
            delta_ms
        );

        ws_broadcast(msg);
    }

    /*
     * HEX -> bytes
     */

    uint8_t bytes[64];

    int byte_len = hex_to_bytes( clean_hex, bytes, sizeof(bytes) );

    if (byte_len <= 0)
    {
        ESP_LOGE(TAG, "PI30 HEX parse error");
        return true;
    }

    /*
     * command_name
     */

    cJSON *cmd_item =
        cJSON_GetObjectItem(root, "command_name");

    const char *command_name =
        cJSON_IsString(cmd_item)
            ? cmd_item->valuestring
            : "UNKNOWN";

    ESP_LOGI( TAG,"📋 PI30 COMMAND: %s",command_name);

    /*
     * RS485 request
     */

    rs485_req_t req = {0};

    memcpy( req.data, bytes, byte_len);
    req.len = byte_len;
    strncpy(
        req.cmd,
        command_name,
        sizeof(req.cmd) - 1
    );

    req.cmd[sizeof(req.cmd) - 1] = '\0';

    /*
     * Сообщаем cloud
     */

    char cloud_msg[512];

    snprintf(
        cloud_msg,
        sizeof(cloud_msg),
        "{"
        "\"command_type\":\"PI30 request received\","
        "\"command_name\":\"%s\","
        "\"hex_data\":\"%s\""
        "}",
        command_name,
        clean_hex
    );

    websocket_send_text(cloud_msg);
    rs485_master_send_req(&req);
    return true;
}



static bool handle_mtcp_data(cJSON *root)
{
    cJSON *ip =
        cJSON_GetObjectItem(root, "ip");

    cJSON *port =
        cJSON_GetObjectItem(root, "port");

    cJSON *unit_id =
        cJSON_GetObjectItem(root, "unit_id");

    cJSON *func =
        cJSON_GetObjectItem(root, "func");

    cJSON *start_addr =
        cJSON_GetObjectItem(root, "start_addr");

    cJSON *quantity =
        cJSON_GetObjectItem(root, "quantity");

    cJSON *value =
        cJSON_GetObjectItem(root, "value");

    if (!cJSON_IsString(ip) ||
        !cJSON_IsNumber(port) ||
        !cJSON_IsNumber(unit_id) ||
        !cJSON_IsNumber(func) ||
        !cJSON_IsNumber(start_addr))
    {
        ESP_LOGE(TAG, "MTCP missing fields");
        return true;
    }

    const char *ip_str = ip->valuestring;

    uint16_t port_v =
        (uint16_t)port->valueint;

    uint8_t unit =
        (uint8_t)unit_id->valueint;

    uint8_t func_v =
        (uint8_t)func->valueint;

    uint16_t start =
        (uint16_t)start_addr->valueint;

    uint16_t qty =
        cJSON_IsNumber(quantity)
            ? (uint16_t)quantity->valueint
            : 0;

    uint16_t val =
        cJSON_IsNumber(value)
            ? (uint16_t)value->valueint
            : 0;

    /*
     * Diagnostics
     */

    if (diagnostics_active)
    {
        char diag_msg[256];

        snprintf(
            diag_msg,
            sizeof(diag_msg),
            "{\"command_type\":\"MTCP request -> "
            "%s:%u unit=%u func=%u start=%u qty=%u val=%u\"}",
            ip_str,
            port_v,
            unit,
            func_v,
            start,
            qty,
            val
        );

        ws_broadcast(diag_msg);
    }

    /*
     * Cloud notification
     */

    char cloud_msg[256];

    snprintf(
        cloud_msg,
        sizeof(cloud_msg),
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
        val
    );

    websocket_send_text(cloud_msg);

    /*
     * Modbus TCP
     */

    static uint8_t resp[256];

    int resp_len = 0;

    esp_err_t err =
        modbus_tcp_request(
            ip_str,
            port_v,
            unit,
            func_v,
            start,
            qty,
            val,
            resp,
            &resp_len
        );

    if (err != ESP_OK)
    {
        return true;
    }

    /*
     * Response -> HEX
     */

    static char hex[520];

    if (resp_len > 0)
    {
        bytes_to_hex(
            resp,
            resp_len,
            hex,
            sizeof(hex)
        );
    }
    else
    {
        hex[0] = '\0';
    }

    cJSON *cmd_name =
        cJSON_GetObjectItem(
            root,
            "command_name"
        );

    const char *name =
        cJSON_IsString(cmd_name)
            ? cmd_name->valuestring
            : "unknown";

    static char ws_msg[1024];

    snprintf(
        ws_msg,
        sizeof(ws_msg),
        "{"
        "\"command_type\":\"modbus_tcp_response\","
        "\"command_name\":\"%s\","
        "\"hex_data\":\"%s\""
        "}",
        name,
        hex
    );

    websocket_send_text(ws_msg);

    return true;
}


static bool handle_msg_data(cJSON *root)
{
    cJSON *status_item =
        cJSON_GetObjectItem(root, "cloud_status");

    if (!cJSON_IsString(status_item) ||
        !status_item->valuestring)
    {
        return false;
    }

    const char *status =
        status_item->valuestring;

    if (strlen(status) >= sizeof(cloud_status_msg))
    {
        ESP_LOGE(TAG, "cloud_status too long");
        return true;
    }

    strcpy(
        cloud_status_msg,
        status
    );

    ESP_LOGI(
        TAG,
        "💾 Saved cloud_status_msg = %s",
        cloud_status_msg
    );

    /*
     * Передаём исходный JSON локальным WS-клиентам.
     *
     * Здесь не используем ws_rx_buf, поскольку dispatcher
     * уже передал нам разобранный JSON.
     */

    char *json =
        cJSON_PrintUnformatted(root);

    if (json)
    {
        ESP_LOGI(TAG, "📡 Broadcasting cloud status to local WS clients: %s", json);
        ws_broadcast(json);
        free(json);
    }

    if (strcmp(cloud_status_msg, "authenticated") == 0 ||
        strcmp(cloud_status_msg, "connected") == 0)
    {
        gpio_set_net_led(true);

        ws_connected = true;

        char time_str[64];

        get_time_iso(
            time_str,
            sizeof(time_str)
        );

        char msg[128];

        snprintf(
            msg,
            sizeof(msg),
            "{\"command_type\":\"device_online\","
            "\"time\":\"%s\"}",
            time_str
        );

        websocket_send_text(msg);
    }
    else
    {
        ws_connected = false;
    }

    return true;
}

static bool handle_traceroute(cJSON *root)
{
    if (!root) {
        return false;
    }

    /* =====================================================
     * Получаем URL / host
     * ===================================================== */

    cJSON *url_item =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "url"
        );

    if (!cJSON_IsString(url_item) ||
        !url_item->valuestring ||
        url_item->valuestring[0] == '\0')
    {
        ESP_LOGE(
            TAG,
            "Traceroute: url is missing"
        );

        websocket_send_text(
            "{"
            "\"command_type\":\"traceroute_error\","
            "\"error\":-1,"
            "\"error_name\":\"missing_url\""
            "}"
        );

        return true;
    }

    const char *host =
        url_item->valuestring;

    ESP_LOGI(
        TAG,
        "Traceroute requested: %s",
        host
    );


    /* =====================================================
     * Проверяем, не запущен ли уже traceroute
     * ===================================================== */

    if (traceroute_is_running())
    {
        ESP_LOGW(
            TAG,
            "Traceroute already running"
        );

        websocket_send_text(
            "{"
            "\"command_type\":\"traceroute_error\","
            "\"error\":-2,"
            "\"error_name\":\"already_running\""
            "}"
        );

        return true;
    }


    /* =====================================================
     * Запуск traceroute
     * ===================================================== */

    esp_err_t err = traceroute_start( host, traceroute_ws_callback, NULL);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "traceroute_start() failed: %s", esp_err_to_name(err));
        char msg[256];
        snprintf(
            msg,
            sizeof(msg),

            "{"
            "\"command_type\":\"traceroute_error\","
            "\"error\":%d,"
            "\"error_name\":\"%s\""
            "}",

            err,
            esp_err_to_name(err)
        );

        websocket_send_text(msg);
        return true;
    }

    ESP_LOGI(TAG,"Traceroute started successfully: %s",host);
    return true;
}



static bool handle_ota_update(
    cJSON *root,
    const char *session_id)
{
    cJSON *url_item = cJSON_GetObjectItem( root, "firmware_url");

    if (!cJSON_IsString(url_item) ||
        !url_item->valuestring)
    {
        ESP_LOGE(TAG, "firmware_url not found");
        return true;
    }

    const char *firmware_url = url_item->valuestring;
    ESP_LOGW( TAG, "🚀 OTA pull from: %s",firmware_url);

    websocket_disable_reconnect();

    char *url_copy = strdup(firmware_url);

    if (!url_copy)
    {
        ESP_LOGE(TAG, "Failed to allocate OTA URL");
        websocket_enable_reconnect();
        return true;
    }

    BaseType_t ret =
        xTaskCreatePinnedToCore( ota_task, "ota_task", 8192, url_copy, 5, NULL, 1);

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(url_copy);
        websocket_enable_reconnect();
        return true;
    }

    return true;
}



