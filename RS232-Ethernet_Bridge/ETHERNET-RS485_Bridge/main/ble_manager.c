#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_manager.h"
#include "nvs_settings.h"

static const char *TAG = "BLE";

static char device_serial[32];
static char ble_device_name[32];
static void ble_start_advertising(void); 
/* ---------- UUIDs (128-bit, little-endian) ---------- */
static const ble_uuid128_t pair_svc_uuid = BLE_UUID128_INIT(
    0x5D,0x4C,0x3B,0x2A,0x1F,0x0E,0x9D,0x8C,
    0x5B,0x4A,0xF6,0xE5,0xD4,0xC3,0xB2,0xA1);
static const ble_uuid128_t pair_rx_uuid  = BLE_UUID128_INIT(
    0x5D,0x4C,0x3B,0x2A,0x1F,0x0E,0x9D,0x8C,
    0x5B,0x4A,0xF6,0xE5,0xD4,0xC3,0xB2,0x91);   // app → device (write)
static const ble_uuid128_t pair_tx_uuid  = BLE_UUID128_INIT(
    0x5D,0x4C,0x3B,0x2A,0x1F,0x0E,0x9D,0x8C,
    0x5B,0x4A,0xF6,0xE5,0xD4,0xC3,0xB2,0x92);   // device → app (read)

/* ---------- State ---------- */
static uint16_t pair_rx_handle;                        // [NEW]
static uint16_t pair_tx_handle;                        // [NEW]
static uint16_t pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_pairing_done = false;           // [NEW]
static volatile bool s_pairing_ok   = false;           // [NEW]
static char s_pair_tx_buf[512];                        // [NEW]

bool ble_pairing_is_done(void)        { return s_pairing_done; }
bool ble_pairing_was_successful(void) { return s_pairing_ok;   }

/* ---------- Build TX JSON ---------- */
static void build_tx_response(const char *status, const char *reason)
{
    cJSON *out = cJSON_CreateObject();
    if (!out) return;
    cJSON_AddStringToObject(out, "status", status);
    if (reason) cJSON_AddStringToObject(out, "reason", reason);
    cJSON_AddStringToObject(out, "device_id", device_serial);
    cJSON_AddStringToObject(out, "ble_name",  ble_device_name);
    cJSON_AddStringToObject(out, "node_name", user_cfg.node_name);
    cJSON_AddNumberToObject(out, "model_id",  MODEL_ID);

    char *js = cJSON_PrintUnformatted(out);
    if (js) {
        strncpy(s_pair_tx_buf, js, sizeof(s_pair_tx_buf)-1);
        s_pair_tx_buf[sizeof(s_pair_tx_buf)-1] = '\0';
        cJSON_free(js);
    }
    cJSON_Delete(out);
}

/* ---------- GATT access ---------- */
static int pairing_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* --- WRITE в RX: телефон присылает login+token --- */
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == pair_rx_handle)
    {
        char rx[512] = {0};
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len >= sizeof(rx)) {
            build_tx_response("error", "payload_too_large");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }
        os_mbuf_copydata(ctxt->om, 0, len, rx);
        rx[len] = '\0';
        ESP_LOGI(TAG, "RX from phone: %s", rx);

        cJSON *in = cJSON_Parse(rx);
        if (!in) {
            build_tx_response("error", "invalid_json");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }

        cJSON *j_login   = cJSON_GetObjectItem(in, "login");
        cJSON *j_token   = cJSON_GetObjectItem(in, "token");
        cJSON *j_expires = cJSON_GetObjectItem(in, "expires");

        /* --- Валидация --- */
        const char *err = NULL;
        if (!cJSON_IsString(j_login) || j_login->valuestring[0] == '\0')        err = "missing_login";
        else if (strlen(j_login->valuestring) >= sizeof(user_cfg.account_login)) err = "login_too_long";
        else if (!cJSON_IsString(j_token) || j_token->valuestring[0] == '\0')    err = "missing_token";
        else if (strlen(j_token->valuestring) >= sizeof(fs_session.session_token)) err = "token_too_long";

        if (err) {
            cJSON_Delete(in);
            ESP_LOGW(TAG, "Pairing validation failed: %s", err);
            build_tx_response("error", err);
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }

        /* --- Сохраняем login в user_cfg --- */
        memset(user_cfg.account_login, 0, sizeof(user_cfg.account_login));
        strncpy(user_cfg.account_login, j_login->valuestring,
                sizeof(user_cfg.account_login)-1);
        nvs_save_user_settings(&user_cfg);

        /* --- Сохраняем token в fs_session --- */
        fs_session_t sess = {0};
        memset(sess.session_token, 0, sizeof(sess.session_token));
        strncpy(sess.session_token, j_token->valuestring,
                sizeof(sess.session_token)-1);
        sess.device_model_id = MODEL_ID;
        sess.expires = cJSON_IsNumber(j_expires)
                     ? (uint32_t)j_expires->valuedouble : 0;

        esp_err_t nerr = nvs_save_fs_session(&sess);
        cJSON_Delete(in);

        if (nerr != ESP_OK) {
            ESP_LOGE(TAG, "nvs_save_fs_session failed: %s", esp_err_to_name(nerr));
            build_tx_response("error", "nvs_save_failed");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }

        ESP_LOGI(TAG, "PAIRING OK: login=%s token=%.8s...",
                 user_cfg.account_login, sess.session_token);

        build_tx_response("ok", NULL);
        s_pairing_ok = true; s_pairing_done = true;
        return 0;
    }

    /* --- READ из TX: телефон забирает ответ --- */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == pair_tx_handle)
    {
        const char *payload = s_pair_tx_buf[0] ? s_pair_tx_buf
                                               : "{\"status\":\"waiting\"}";
        int rc = os_mbuf_append(ctxt->om, payload, strlen(payload));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

/* ---------- GATT table ---------- */
static const struct ble_gatt_svc_def pairing_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &pair_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &pair_rx_uuid.u, .access_cb = pairing_gatt_access,
              .flags = BLE_GATT_CHR_F_WRITE, .val_handle = &pair_rx_handle },
            { .uuid = &pair_tx_uuid.u, .access_cb = pairing_gatt_access,
              .flags = BLE_GATT_CHR_F_READ,  .val_handle = &pair_tx_handle },
            { 0 }
        },
    },
    { 0 }
};

/* ---------- GAP events ---------- */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            pair_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Phone connected, conn=%d", pair_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
            ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Phone disconnected, reason=%d", event->disconnect.reason);
        pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (!s_pairing_done) ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_pairing_done) ble_start_advertising();
        return 0;
    default:
        return 0;
    }
}

/* ---------- Identity ---------- */
static void ble_prepare_device_identity(void)
{
    generate_device_serial(device_serial, sizeof(device_serial));
    snprintf(ble_device_name, sizeof(ble_device_name),"%.26s", device_serial);
    ESP_LOGI(TAG, "Device serial: %s", device_serial);
    ESP_LOGI(TAG, "BLE device name: %s", ble_device_name);
}

/* ---------- Host task ---------- */
static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
    ESP_LOGI(TAG, "NimBLE host task stopped");
    vTaskDelete(NULL);
}

/* ---------- Advertising ---------- */
static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params adv_params = {0};

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_device_name;
    fields.name_len = strlen(ble_device_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields: %d", rc); return; }

    adv_params.itvl_min = 160;
    adv_params.itvl_max = 160;
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* [NEW] передаём обработчик событий */
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start: %d", rc); return; }

    ESP_LOGI(TAG, "BLE advertising started as '%s'", ble_device_name);
}

/* ---------- Sync callback ---------- */
static void ble_on_sync(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed");
        return;
    }
    uint8_t addr[6] = {0};
    if (ble_hs_id_copy_addr(own_addr_type, addr, NULL) == 0) {
        ESP_LOGI(TAG, "BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5],addr[4],addr[3],addr[2],addr[1],addr[0]);
    }
    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

/* ---------- Init (вызывается 1 раз из pairing_manager) ---------- */
esp_err_t ble_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE / NimBLE");
    ble_prepare_device_identity();

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* [NEW] регистрируем GATT pairing-сервис */
    int rc = ble_gatts_count_cfg(pairing_gatt_svcs);
    if (rc) { ESP_LOGE(TAG, "ble_gatts_count_cfg: %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(pairing_gatt_svcs);
    if (rc) { ESP_LOGE(TAG, "ble_gatts_add_svcs: %d", rc); return ESP_FAIL; }

    rc = ble_svc_gap_device_name_set(ble_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "NimBLE initialized");
    return ESP_OK;
}

/* ---------- Публичный API pairing ---------- */
esp_err_t ble_start_pairing_mode(void)
{
    s_pairing_done   = false;
    s_pairing_ok     = false;
    s_pair_tx_buf[0] = '\0';
    pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_err_t err = ble_manager_init();
    if (err != ESP_OK) return err;

    /* Готовим "waiting"-ответ, чтобы телефон мог прочитать инфу до записи */
    build_tx_response("waiting", NULL);
    ESP_LOGI(TAG, "BLE pairing mode started, waiting for phone...");
    return ESP_OK;
}

void ble_stop_pairing_mode(void)
{
    if (pair_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(pair_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    nimble_port_deinit();
}


