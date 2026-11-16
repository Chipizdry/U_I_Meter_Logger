

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define EVT_LOG_RING_SIZE   32
#define EVT_LOG_NVS_NS      "evt_log"
#define EVT_LOG_MAGIC       0x45564C47UL  // "EVLG"

typedef enum {
    EVT_BOOT              = 1,
    EVT_WDT_REBOOT        = 2,
    EVT_PANIC             = 3,
    EVT_BROWNOUT          = 4,
    EVT_FACTORY_RESET     = 5,
    EVT_SW_RESET          = 6,

    EVT_WS_CONNECTED      = 10,
    EVT_WS_DISCONNECTED   = 11,
    EVT_WS_ERROR          = 12,
    EVT_WS_SOFT_RESTART   = 13,
    EVT_WS_STUCK          = 14,
    EVT_WS_AUTH_OK        = 15,
    EVT_WS_AUTH_FAIL      = 16,

    EVT_BACKEND_SILENT    = 20,
    EVT_HEARTBEAT_LOST    = 21,

    EVT_ETH_UP            = 30,
    EVT_ETH_DOWN          = 31,
    EVT_WIFI_UP           = 32,
    EVT_WIFI_DOWN         = 33,
    EVT_NET_READY         = 34,
    EVT_NET_LOST          = 35,
    EVT_DEF_NETIF_LOST    = 36,
    EVT_FAILOVER_SWITCH   = 37,

    EVT_HEAP_LOW          = 40,
    EVT_HEAP_CRITICAL     = 41,
    EVT_SNTP_SYNCED       = 42,

    EVT_OTA_START         = 50,
    EVT_OTA_DONE          = 51,
    EVT_OTA_FAIL          = 52,
} evt_type_t;

typedef struct {
    uint32_t uptime_sec;
    uint32_t epoch_sec;
    uint16_t seq;
    uint16_t boot_id;
    uint16_t p1;
    uint16_t p2;
    uint8_t  type;
    uint8_t  reason;
    uint8_t  _pad[2];
} evt_rec_t;    // 20 байт

esp_err_t evt_log_init(void);
void      evt_log (evt_type_t type, uint8_t reason, uint16_t p1);
void      evt_log2(evt_type_t type, uint8_t reason, uint16_t p1, uint16_t p2);
void      evt_log_flush(void);

size_t evt_log_read(evt_rec_t *out, size_t max_count, uint16_t since_seq);
void   evt_log_dump(void);
char  *evt_log_to_json(size_t max_count);

const char *evt_type_str(evt_type_t t);
const char *evt_reset_reason_str(uint8_t r);

void     evt_mark_ws_rx(void);
void     evt_mark_backend_request(void);
void     evt_mark_heartbeat(void);
uint32_t evt_sec_since_ws_rx(void);
uint32_t evt_sec_since_backend_request(void);
uint32_t evt_sec_since_heartbeat(void);

uint16_t evt_boot_id(void);



