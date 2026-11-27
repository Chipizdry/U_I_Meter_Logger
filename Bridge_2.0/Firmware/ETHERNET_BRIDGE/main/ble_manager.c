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
static uint16_t pair_rx_handle;
static uint16_t pair_tx_handle;
static uint16_t pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_pairing_done = false;
static volatile bool s_pairing_ok   = false;
static char s_pair_tx_buf[512];

/* RX-буфер вынесен из стека задачи NimBLE (в стеке 384 байта + cJSON_Parse
 * + ESP_LOGI → переполнение 4096-байтного стека → порча кучи → crash).
 * access_cb вызывается последовательно из одной задачи NimBLE — гонок нет. */
static char s_pair_rx_buf[512];

bool ble_pairing_is_done(void)        { return s_pairing_done; }
bool ble_pairing_was_successful(void) { return s_pairing_ok;   }

/* Возвращает указатель на строку или NULL, если поле не string/пустое/повреждено.
 * Единственная безопасная точка доступа к cJSON->valuestring. */
static const char *safe_cjstr(cJSON *j)
{
    if (!j || !cJSON_IsString(j) || !j->valuestring)
        return NULL;
    return j->valuestring;
}

/* ---------- Build TX JSON ----------
 * secret_key берётся из fs_session (загружен из NVS в main.c при boot,
 * записан на заводе). Здесь только читаем и отдаём телефону. */
static void build_tx_response(const char *status, const char *reason)
{
    cJSON *out = cJSON_CreateObject();
    if (!out) return;
    cJSON_AddStringToObject(out, "status",     status);
    if (reason)
        cJSON_AddStringToObject(out, "reason", reason);
    cJSON_AddStringToObject(out, "device_id",  device_serial);
    cJSON_AddStringToObject(out, "ble_name",   ble_device_name);
    cJSON_AddStringToObject(out, "node_name",  user_cfg.node_name);
    cJSON_AddStringToObject(out, "secret_key", fs_session.secret_key);
    cJSON_AddNumberToObject(out, "model_id",   MODEL_ID);

    char *js = cJSON_PrintUnformatted(out);
    if (js) {
        strncpy(s_pair_tx_buf, js, sizeof(s_pair_tx_buf) - 1);
        s_pair_tx_buf[sizeof(s_pair_tx_buf) - 1] = '\0';
        cJSON_free(js);
    }
    cJSON_Delete(out);
}

/* ---------- GATT access ---------- */
static int pairing_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* ============ WRITE в RX ============ */
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == pair_rx_handle)
    {
        if (s_pairing_done) return 0;                    // уже обработали

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len >= sizeof(s_pair_rx_buf)) {
            ESP_LOGW(TAG, "payload_too_large: %u", len);
            build_tx_response("error", "payload_too_large");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }

        memset(s_pair_rx_buf, 0, sizeof(s_pair_rx_buf));
        os_mbuf_copydata(ctxt->om, 0, len, s_pair_rx_buf);
        s_pair_rx_buf[len] = '\0';
        ESP_LOGI(TAG, "RX from phone (%u bytes): %s", len, s_pair_rx_buf);

        cJSON *in = cJSON_Parse(s_pair_rx_buf);
        if (!in) {
            ESP_LOGW(TAG, "invalid_json");
            build_tx_response("error", "invalid_json");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }

        /* --- Извлекаем все поля как безопасные C-строки --- */
        const char *s_login   = safe_cjstr(cJSON_GetObjectItem(in, "login"));
        const char *s_token   = safe_cjstr(cJSON_GetObjectItem(in, "token"));
        const char *s_accpass = safe_cjstr(cJSON_GetObjectItem(in, "account_password"));
        const char *s_ap_ssid = safe_cjstr(cJSON_GetObjectItem(in, "ap_ssid"));
        const char *s_ap_pass = safe_cjstr(cJSON_GetObjectItem(in, "ap_password"));
        cJSON      *j_expires = cJSON_GetObjectItem(in, "expires");

        /* --- Предупреждаем о неизвестных полях (на будущее) --- */
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, in) {
            const char *k = it->string;
            if (!k) continue;
            if (!strcmp(k, "login")            || !strcmp(k, "token")    ||
                !strcmp(k, "account_password") || !strcmp(k, "ap_ssid")  ||
                !strcmp(k, "ap_password")      || !strcmp(k, "expires")) continue;
            ESP_LOGW(TAG, "RX unknown field ignored: '%s'", k);
        }

        /* --- Валидация --- */
        const char *err = NULL;
        if (!s_login || s_login[0] == '\0')
            err = "missing_login";
        else if (strlen(s_login) >= sizeof(user_cfg.account_login))
            err = "login_too_long";
        else if (!s_token || s_token[0] == '\0')
            err = "missing_token";
        else if (strlen(s_token) >= sizeof(fs_session.session_token))
            err = "token_too_long";
        else if (s_accpass && strlen(s_accpass) >= sizeof(user_cfg.account_password))
            err = "account_password_too_long";
        else if (s_ap_ssid && s_ap_ssid[0] &&
                 strlen(s_ap_ssid) >= sizeof(wifi_cfg.sta_ssid))
            err = "ap_ssid_too_long";
        else if (s_ap_pass && s_ap_pass[0]) {
            size_t pl = strlen(s_ap_pass);
            if (pl < 8)
                err = "ap_password_too_short";
            else if (pl >= sizeof(wifi_cfg.sta_password))
                err = "ap_password_too_long";
        }

        if (err) {
            cJSON_Delete(in);
            ESP_LOGW(TAG, "Validation failed: %s", err);
            build_tx_response("error", err);
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }

        ESP_LOGI(TAG, "Validation passed: login=%s token=%.8s...",
                 s_login, s_token);

        /* ============ СОХРАНЕНИЕ ============ */

        /* 1) user_cfg: login + (опц.) account_password */
        memset(user_cfg.account_login, 0, sizeof(user_cfg.account_login));
        strncpy(user_cfg.account_login, s_login,sizeof(user_cfg.account_login) - 1);

        if (s_accpass && s_accpass[0]) {
            memset(user_cfg.account_password, 0, sizeof(user_cfg.account_password));
            strncpy(user_cfg.account_password, s_accpass,sizeof(user_cfg.account_password) - 1);
        }

        esp_err_t e_user = nvs_save_user_settings(&user_cfg);
        if (e_user != ESP_OK) {
            ESP_LOGE(TAG, "nvs_save_user_settings failed: %s",
                     esp_err_to_name(e_user));
            cJSON_Delete(in);
            build_tx_response("error", "nvs_save_failed");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }
        ESP_LOGI(TAG, "Saved user_cfg");

        /* 2) fs_session: token, expires.
         * secret_key остаётся нетронутым — он уже в fs_session из NVS
         * (зашит на заводе), сохраняем вместе с остальными полями. */
        memset(fs_session.session_token, 0, sizeof(fs_session.session_token));
        strncpy(fs_session.session_token, s_token,
                sizeof(fs_session.session_token) - 1);
        fs_session.device_model_id = MODEL_ID;
        if (cJSON_IsNumber(j_expires))
            fs_session.expires = (uint32_t)j_expires->valuedouble;

        esp_err_t e_sess = nvs_save_session_settings(&fs_session);
        if (e_sess != ESP_OK) {
            ESP_LOGE(TAG, "nvs_save_session_settings failed: %s",
                     esp_err_to_name(e_sess));
            cJSON_Delete(in);
            build_tx_response("error", "nvs_save_failed");
            s_pairing_ok = false; s_pairing_done = true;
            return 0;
        }
        ESP_LOGI(TAG, "Saved session");

        /* 3) wifi_cfg — только если поля пришли и не пустые */
        bool wifi_changed = false;
        if (s_ap_ssid && s_ap_ssid[0]) {
            memset(wifi_cfg.sta_ssid, 0, sizeof(wifi_cfg.sta_ssid));
            strncpy(wifi_cfg.sta_ssid, s_ap_ssid,
                    sizeof(wifi_cfg.sta_ssid) - 1);
            wifi_changed = true;
        }
        if (s_ap_pass && s_ap_pass[0]) {
            memset(wifi_cfg.sta_password, 0, sizeof(wifi_cfg.sta_password));
            strncpy(wifi_cfg.sta_password, s_ap_pass,
                    sizeof(wifi_cfg.sta_password) - 1);
            wifi_changed = true;
        }
        if (wifi_changed) {
            esp_err_t e_wifi = nvs_save_wifi_settings(&wifi_cfg);
            if (e_wifi != ESP_OK)
                ESP_LOGE(TAG, "nvs_save_wifi_settings failed: %s",
                         esp_err_to_name(e_wifi));
            else
                ESP_LOGI(TAG, "Saved wifi_cfg");
        }

        /* Все s_* прочитаны и сохранены — теперь освобождаем cJSON */
        cJSON_Delete(in);

        ESP_LOGI(TAG, "PAIRING OK: login=%s token=%.8s... sta_ssid=%s",
                 user_cfg.account_login,
                 fs_session.session_token,
                 wifi_cfg.sta_ssid);

        build_tx_response("ok", NULL);
        s_pairing_ok = true; s_pairing_done = true;
        return 0;
    }

    /* ============ READ из TX ============ */
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
    snprintf(ble_device_name, sizeof(ble_device_name), "%.26s", device_serial);
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

    fields.flags           = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name            = (uint8_t *)ble_device_name;
    fields.name_len        = strlen(ble_device_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields: %d", rc); return; }

    adv_params.itvl_min  = 160;
    adv_params.itvl_max  = 160;
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

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
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }
    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

/* ---------- Init ---------- */
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

/* ---------- Публичный API ---------- */
esp_err_t ble_start_pairing_mode(void)
{
    s_pairing_done   = false;
    s_pairing_ok     = false;
    s_pair_tx_buf[0] = '\0';
    pair_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    /* secret_key уже в fs_session (загружен из NVS при boot, зашит на заводе).
     * Здесь ничего генерировать не нужно — build_tx_response прочитает его. */
    if (fs_session.secret_key[0] == '\0') {
        ESP_LOGW(TAG, "secret_key is empty in NVS — check factory provisioning");
    } else {
        ESP_LOGI(TAG, "Using factory secret_key: %.8s...", fs_session.secret_key);
    }

    esp_err_t err = ble_manager_init();
    if (err != ESP_OK) return err;

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



