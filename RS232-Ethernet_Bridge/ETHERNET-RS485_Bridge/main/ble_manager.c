

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"



#include "ble_manager.h"


static const char *TAG = "BLE";


/* ============================================================
 * BLE Host Task
 * ============================================================ */

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");

    /*
     * Main NimBLE event loop.
     *
     * This function normally does not return until
     * nimble_port_stop() is called.
     */
    nimble_port_run();

    nimble_port_freertos_deinit();

    ESP_LOGI(TAG, "NimBLE host task stopped");

    vTaskDelete(NULL);
}


/* ============================================================
 * GAP callbacks
 * ============================================================ */

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}


/* ============================================================
 * Start advertising
 * ============================================================ */

static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;

    memset(&fields, 0, sizeof(fields));

    /*
     * General discoverable mode.
     */
    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;

    /*
     * Device name.
     *
     * For the first test we use a fixed name.
     * Later we will generate:
     *
     * COR-Bridge-XXXXXXXX
     */
    const char *name = "COR-Bridge";

    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /*
     * Configure advertising fields.
     */
    int rc = ble_gap_adv_set_fields(&fields);

    if (rc != 0) {
        ESP_LOGE(TAG,
                 "ble_gap_adv_set_fields() failed: %d",
                 rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));

    /*
     * Advertising interval.
     *
     * 160 * 0.625 ms = 100 ms
     */
    adv_params.itvl_min = 160;
    adv_params.itvl_max = 160;

    /*
     * Undirected connectable advertising.
     */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;

    /*
     * General discoverable.
     */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        BLE_OWN_ADDR_PUBLIC,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        NULL,
        NULL
    );

    if (rc != 0) {
        ESP_LOGE(TAG,
                 "ble_gap_adv_start() failed: %d",
                 rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started");
    ESP_LOGI(TAG, "Device name: %s", name);
}


/* ============================================================
 * NimBLE synchronization callback
 * ============================================================ */

static void ble_on_sync(void)
{
    int rc;

    uint8_t own_addr_type;

    /*
     * Determine the best BLE address type.
     */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);

    if (rc != 0) {
        ESP_LOGE(TAG,
                 "ble_hs_id_infer_auto() failed: %d",
                 rc);
        return;
    }

    /*
     * Print BLE MAC address.
     */
    uint8_t addr_val[6] = {0};

    rc = ble_hs_id_copy_addr(
        own_addr_type,
        addr_val,
        NULL
    );

    if (rc == 0) {

        ESP_LOGI(
            TAG,
            "BLE address: %02X:%02X:%02X:%02X:%02X:%02X",
            addr_val[5],
            addr_val[4],
            addr_val[3],
            addr_val[2],
            addr_val[1],
            addr_val[0]
        );
    }

    /*
     * Start advertising after NimBLE host
     * synchronization.
     */
    ble_start_advertising();
}


/* ============================================================
 * BLE initialization
 * ============================================================ */

esp_err_t ble_manager_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Initializing BLE / NimBLE");
    ESP_LOGI(TAG, "========================================");


    /*
     * Initialize NimBLE host/controller integration.
     *
     * ESP-IDF 5.x handles the BLE controller integration
     * through NimBLE initialization.
     */
    esp_err_t ret = nimble_port_init();

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "nimble_port_init() failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    /*
     * Configure NimBLE host.
     */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;


    /*
     * Initialize standard GAP service.
     */
    ble_svc_gap_init();


    /*
     * Initialize standard GATT service.
     */
    ble_svc_gatt_init();


    /*
     * Set device name.
     */
    int rc = ble_svc_gap_device_name_set("COR-Bridge");

    if (rc != 0) {

        ESP_LOGE(
            TAG,
            "ble_svc_gap_device_name_set() failed: %d",
            rc
        );

        return ESP_FAIL;
    }


    /*
     * Initialize NimBLE persistent storage.
     *
     * Later this will be important for BLE bonding,
     * encryption keys and paired devices.
     */
   // ble_store_config_init();


    /*
     * Start NimBLE host task.
     */
    nimble_port_freertos_init(ble_host_task);


    ESP_LOGI(TAG, "NimBLE initialized");

    return ESP_OK;
}


