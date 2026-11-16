

#include "event_log.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "EVT";

/* =====================================================================
 * Внутренние пороги (все в одном месте)
 * ===================================================================== */
#define EVT_FLUSH_PERIOD_MS       (10 * 60 * 1000)   // 10 минут
#define EVT_MIN_COMMIT_GAP_SEC    15                 // не чаще 1 commit / 15 сек
#define EVT_DEDUP_WINDOW_SEC      300                // 5 минут
#define EVT_FLUSH_MIN_FREE_HEAP   6000               // не трогаем flash если heap < 6 KB

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t seq_counter;
    uint16_t boot_counter;
    uint16_t flushed_seq;
    uint32_t last_seen_uptime;
    uint16_t _pad;
} evt_meta_t;

/* =====================================================================
 * Состояние
 * ===================================================================== */
static evt_rec_t        s_ring[EVT_LOG_RING_SIZE];
static size_t           s_head;
static size_t           s_count;
static uint16_t         s_seq_counter;
static uint16_t         s_boot_id;
static uint16_t         s_flushed_seq;
static uint32_t         s_last_commit_sec;
static SemaphoreHandle_t s_mutex;
static bool             s_ready;

static volatile uint32_t s_last_ws_rx_sec;
static volatile uint32_t s_last_backend_sec;
static volatile uint32_t s_last_heartbeat_sec;
static uint32_t s_last_seen_uptime;
static uint32_t s_last_saved_uptime;
static inline uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

/* =====================================================================
 * Строки
 * ===================================================================== */
const char *evt_type_str(evt_type_t t)
{
    switch (t) {
        case EVT_BOOT: return "BOOT";
        case EVT_WDT_REBOOT: return "WDT_REBOOT";
        case EVT_PANIC: return "PANIC";
        case EVT_BROWNOUT: return "BROWNOUT";
        case EVT_FACTORY_RESET: return "FACTORY_RESET";
        case EVT_SW_RESET: return "SW_RESET";
        case EVT_WS_CONNECTED: return "WS_CONNECTED";
        case EVT_WS_DISCONNECTED: return "WS_DISCONNECTED";
        case EVT_WS_ERROR: return "WS_ERROR";
        case EVT_WS_SOFT_RESTART: return "WS_SOFT_RESTART";
        case EVT_WS_STUCK: return "WS_STUCK";
        case EVT_WS_AUTH_OK: return "WS_AUTH_OK";
        case EVT_WS_AUTH_FAIL: return "WS_AUTH_FAIL";
        case EVT_BACKEND_SILENT: return "BACKEND_SILENT";
        case EVT_HEARTBEAT_LOST: return "HEARTBEAT_LOST";
        case EVT_ETH_UP: return "ETH_UP";
        case EVT_ETH_DOWN: return "ETH_DOWN";
        case EVT_WIFI_UP: return "WIFI_UP";
        case EVT_WIFI_DOWN: return "WIFI_DOWN";
        case EVT_NET_READY: return "NET_READY";
        case EVT_NET_LOST: return "NET_LOST";
        case EVT_DEF_NETIF_LOST: return "DEF_NETIF_LOST";
        case EVT_FAILOVER_SWITCH: return "FAILOVER_SWITCH";
        case EVT_HEAP_LOW: return "HEAP_LOW";
        case EVT_HEAP_CRITICAL: return "HEAP_CRITICAL";
        case EVT_SNTP_SYNCED: return "SNTP_SYNCED";
        case EVT_OTA_START: return "OTA_START";
        case EVT_OTA_DONE: return "OTA_DONE";
        case EVT_OTA_FAIL: return "OTA_FAIL";
        default: return "?";
    }
}

const char *evt_reset_reason_str(uint8_t r)
{
    switch (r) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default: return "?";
    }
}

/* =====================================================================
 * NVS helpers — все *_locked() БЕЗ mutex (вызываются только под ним)
 * ===================================================================== */
static void meta_load(evt_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    nvs_handle_t h;
    if (nvs_open(EVT_LOG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(*m);
    nvs_get_blob(h, "meta", m, &sz);
    nvs_close(h);
    if (m->magic != EVT_LOG_MAGIC) memset(m, 0, sizeof(*m));
}

static void meta_save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(EVT_LOG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    evt_meta_t m = {
        .magic            = EVT_LOG_MAGIC,
        .version          = 3,
        .seq_counter      = s_seq_counter,
        .boot_counter     = s_boot_id,
        .flushed_seq      = s_flushed_seq,
        .last_seen_uptime = s_last_seen_uptime
    };
    nvs_set_blob(h, "meta", &m, sizeof(m));
    nvs_commit(h);
    nvs_close(h);
}

static void load_ring_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(EVT_LOG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (size_t i = 0; i < EVT_LOG_RING_SIZE; i++) {
        char key[8];
        snprintf(key, sizeof(key), "e%u", (unsigned)i);
        size_t sz = sizeof(evt_rec_t);
        nvs_get_blob(h, key, &s_ring[i], &sz);
    }
    nvs_close(h);
}

/* Записать всё, что ещё не во flash. Вызывается ТОЛЬКО под s_mutex. */
static void flush_dirty_locked(void)
{
    /* 1) Обновляем last_seen_uptime текущим uptime — это ключ к boot-loop detection */
    uint32_t now_up = now_sec();
    if (now_up > s_last_seen_uptime) {
        s_last_seen_uptime = now_up;
    }

    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < EVT_FLUSH_MIN_FREE_HEAP) {
        ESP_LOGW(TAG, "flush skip: heap too low");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(EVT_LOG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "flush: NVS open failed");
        return;
    }

    size_t written = 0;
    uint16_t new_flushed = s_flushed_seq;
    for (size_t i = 0; i < s_count; i++) {
        size_t idx = (s_head + EVT_LOG_RING_SIZE - s_count + i) % EVT_LOG_RING_SIZE;
        evt_rec_t *r = &s_ring[idx];
        if (r->seq == 0) continue;
        if ((int16_t)(r->seq - s_flushed_seq) <= 0) continue;
        char key[8];
        snprintf(key, sizeof(key), "e%u", (unsigned)idx);
        if (nvs_set_blob(h, key, r, sizeof(*r)) != ESP_OK) continue;
        if (r->seq > new_flushed) new_flushed = r->seq;
        written++;
    }

    /* 2) Сохраняем meta, если записали новые records ИЛИ last_seen_uptime сдвинулся */
    bool meta_dirty = (written > 0) || (s_last_seen_uptime != s_last_saved_uptime);
    if (meta_dirty) {
        evt_meta_t m = {
            .magic            = EVT_LOG_MAGIC,
            .version          = 3,                      // ← критично
            .seq_counter      = s_seq_counter,
            .boot_counter     = s_boot_id,
            .flushed_seq      = new_flushed,
            .last_seen_uptime = s_last_seen_uptime
        };
        nvs_set_blob(h, "meta", &m, sizeof(m));
        nvs_commit(h);
        if (written > 0) s_flushed_seq = new_flushed;
        s_last_saved_uptime = s_last_seen_uptime;
        ESP_LOGD(TAG, "flushed %u records, meta up=%us",
                 (unsigned)written, (unsigned)s_last_seen_uptime);
    }
    nvs_close(h);
}

/* =====================================================================
 * Дедупликация и throttle
 * ===================================================================== */
static bool type_should_dedup(evt_type_t t)
{
    switch (t) {
        case EVT_WS_DISCONNECTED:
        case EVT_WS_ERROR:
        case EVT_ETH_DOWN:
        case EVT_WIFI_DOWN:
        case EVT_HEAP_LOW:
        case EVT_NET_LOST:
        case EVT_DEF_NETIF_LOST:
            return true;
        default:
            return false;
    }
}

static bool type_is_critical(evt_type_t t)
{
    switch (t) {
        case EVT_BOOT:
        case EVT_WDT_REBOOT:
        case EVT_PANIC:
        case EVT_BROWNOUT:
        case EVT_FACTORY_RESET:
        case EVT_OTA_DONE:
            return true;
        default:
            return false;
    }
}

/* Вызывается под s_mutex. Возвращает true, если запись уже учтена в ring
 * как инкремент счётчика и новую создавать не надо. */
static bool try_dedup_locked(evt_type_t type, uint8_t reason)
{
    if (!type_should_dedup(type)) return false;
    if (s_count == 0) return false;

    size_t last_idx = (s_head + EVT_LOG_RING_SIZE - 1) % EVT_LOG_RING_SIZE;
    evt_rec_t *last = &s_ring[last_idx];
    if (last->seq == 0) return false;
    if (last->type != (uint8_t)type) return false;
    if (last->reason != reason) return false;
    if (now_sec() - last->uptime_sec >= EVT_DEDUP_WINDOW_SEC) return false;

    if (last->p1 < 0xFFFF) last->p1++;

    /* Обновим last_seen — событие реально случилось, сессия жива */
    uint32_t now_up = now_sec();
    if (now_up > s_last_seen_uptime) s_last_seen_uptime = now_up;

    return true;
}

/* Вызывается под s_mutex. Только для critical: commit не чаще чем раз в
 * EVT_MIN_COMMIT_GAP_SEC — защита от boot-loop, убивающего flash. */
static void maybe_flush_critical_locked(evt_type_t type)
{
    if (!type_is_critical(type)) return;
    uint32_t now = now_sec();
    if (s_last_commit_sec && (now - s_last_commit_sec) < EVT_MIN_COMMIT_GAP_SEC) {
        return;
    }
    s_last_commit_sec = now;
    flush_dirty_locked();
}

/* =====================================================================
 * Периодическая задача flush
 * ===================================================================== */
static void evt_flush_task(void *arg)
{
    ESP_LOGI(TAG, "flush task started (period=%d ms)", EVT_FLUSH_PERIOD_MS);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EVT_FLUSH_PERIOD_MS));
        evt_log_flush();
    }
}

/* =====================================================================
 * Public: init
 * ===================================================================== */
esp_err_t evt_log_init(void)
{
    if (s_ready) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    evt_meta_t m;
    meta_load(&m);

    /* Миграция v0 → v2 (сломаная схема ключей) */
    if (m.magic == EVT_LOG_MAGIC && m.version < 2) {
        nvs_handle_t h;
        if (nvs_open(EVT_LOG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h); nvs_commit(h); nvs_close(h);
        }
        ESP_LOGW(TAG, "evt_log: migrate schema v%u -> v2 (erased)", (unsigned)m.version);
        memset(&m, 0, sizeof(m));
    }

    /* Миграция v2 → v3 (добавлено last_seen_uptime) */
    if (m.magic == EVT_LOG_MAGIC && m.version == 2) {
        nvs_handle_t h;
        if (nvs_open(EVT_LOG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h); nvs_commit(h); nvs_close(h);
        }
        ESP_LOGW(TAG, "evt_log: migrate schema v2 -> v3 (erased)");
        memset(&m, 0, sizeof(m));
    }

    s_seq_counter      = m.seq_counter;
    s_boot_id          = m.boot_counter + 1;
    s_flushed_seq      = m.flushed_seq;
    s_last_seen_uptime = m.last_seen_uptime;     
    s_last_saved_uptime = m.last_seen_uptime;
    load_ring_from_nvs();

    if (s_seq_counter > 0) {
        s_count = (s_seq_counter > EVT_LOG_RING_SIZE) ? EVT_LOG_RING_SIZE : s_seq_counter;
        s_head  = s_seq_counter % EVT_LOG_RING_SIZE;
    }

    s_ready = true;
    meta_save_locked();

    /* ПРАВИЛЬНЫЙ boot-loop detect: смотрим, сколько прожила предыдущая
     * сессия (max uptime), а не uptime BOOT-записи (тот всегда ~0). */
    bool boot_loop = false;
    if (m.boot_counter > 0 && s_last_seen_uptime < 15) {
        boot_loop = true;
        ESP_LOGW(TAG, "BOOT-LOOP: previous session lived only %us",
                 (unsigned)s_last_seen_uptime);
    }

    esp_reset_reason_t rr = esp_reset_reason();
    uint16_t heap_kb = (uint16_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    if (!boot_loop) {
        evt_log(EVT_BOOT, (uint8_t)rr, heap_kb);
    } else {
        /* Накапливаем счётчик в последней BOOT-записи */
        if (s_count > 0) {
            size_t li = (s_head + EVT_LOG_RING_SIZE - 1) % EVT_LOG_RING_SIZE;
            evt_rec_t *last = &s_ring[li];
            if (last->type == EVT_BOOT && last->p1 < 0xFFFF) {
                last->p1++;
                if ((int16_t)(last->seq - s_flushed_seq) <= 0) {
                    s_flushed_seq = last->seq - 1;
                }
            }
        }
    }

    ESP_LOGW(TAG, "boot #%u reset=%s heap=%ukB prev_seen_uptime=%us%s",
             s_boot_id, evt_reset_reason_str((uint8_t)rr), heap_kb,
             (unsigned)m.last_seen_uptime,
             boot_loop ? "  [BOOT-LOOP]" : "");

    evt_log_dump();
    xTaskCreate(evt_flush_task, "evt_flush", 3072, NULL, 3, NULL);
    return ESP_OK;
}

/* =====================================================================
 * Public: запись
 * ===================================================================== */
void evt_log(evt_type_t type, uint8_t reason, uint16_t p1)
{
    evt_log2(type, reason, p1, 0);
}

void evt_log2(evt_type_t type, uint8_t reason, uint16_t p1, uint16_t p2)
{
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    /* Дедупликация: если такое же событие было < 5 мин назад — инкрементим p1 */
    if (try_dedup_locked(type, reason)) {
        maybe_flush_critical_locked(type);
        xSemaphoreGive(s_mutex);
        return;
    }

    evt_rec_t r = {0};
    r.uptime_sec = now_sec();
    time_t t = time(NULL);
    r.epoch_sec  = (t > 1600000000) ? (uint32_t)t : 0;
    r.seq        = ++s_seq_counter;
    r.boot_id    = s_boot_id;
    r.type       = (uint8_t)type;
    r.reason     = reason;
    r.p1         = p1;
    r.p2         = p2;

    if (r.uptime_sec > s_last_seen_uptime) s_last_seen_uptime = r.uptime_sec;
    s_ring[s_head] = r;
    s_head = (s_head + 1) % EVT_LOG_RING_SIZE;
    if (s_count < EVT_LOG_RING_SIZE) s_count++;

    maybe_flush_critical_locked(type);
    xSemaphoreGive(s_mutex);
}

void evt_log_flush(void)
{
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    flush_dirty_locked();
    xSemaphoreGive(s_mutex);
}

/* =====================================================================
 * Public: чтение
 * ===================================================================== */
size_t evt_log_read(evt_rec_t *out, size_t max_count, uint16_t since_seq)
{
    if (!s_ready || !out || !max_count) return 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;

    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max_count; i++) {
        size_t idx = (s_head + EVT_LOG_RING_SIZE - 1 - i) % EVT_LOG_RING_SIZE;
        evt_rec_t *r = &s_ring[idx];
        if (r->seq == 0) continue;
        if (since_seq && (int16_t)(r->seq - since_seq) <= 0) continue;
        out[n++] = *r;
    }
    xSemaphoreGive(s_mutex);
    return n;
}

void evt_log_dump(void)
{
    evt_rec_t snap[EVT_LOG_RING_SIZE];
    size_t n = evt_log_read(snap, EVT_LOG_RING_SIZE, 0);
    ESP_LOGW(TAG, "=== EVENT LOG (%u records, boot=%u) ===",
             (unsigned)n, s_boot_id);
    for (size_t i = 0; i < n; i++) {
        evt_rec_t *r = &snap[i];
        ESP_LOGW(TAG, "#%u boot=%u up=%lus ep=%lu %s(rs=%u) p1=%u p2=%u",
                 r->seq, r->boot_id, (unsigned long)r->uptime_sec,
                 (unsigned long)r->epoch_sec,
                 evt_type_str((evt_type_t)r->type),
                 r->reason, r->p1, r->p2);
    }
    ESP_LOGW(TAG, "=== /EVENT LOG ===");
}

char *evt_log_to_json(size_t max_count)
{
    if (!s_ready) return NULL;
    if (max_count == 0 || max_count > EVT_LOG_RING_SIZE) max_count = EVT_LOG_RING_SIZE;

    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8000) {
        ESP_LOGW(TAG, "evt_log_to_json: heap low");
        return NULL;
    }

    evt_rec_t *snap = malloc(EVT_LOG_RING_SIZE * sizeof(evt_rec_t));
    if (!snap) return NULL;
    size_t n = evt_log_read(snap, max_count, 0);

    size_t cap = 128 + n * 160;
    char *buf = malloc(cap);
    if (!buf) { free(snap); return NULL; }

    size_t pos = 0;
    pos += snprintf(buf + pos, cap - pos,
                    "{\"command_type\":\"event_log\",\"boot_id\":%u,"
                    "\"count\":%u,\"events\":[",
                    s_boot_id, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        evt_rec_t *r = &snap[i];
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, cap - pos,
            "{\"seq\":%u,\"boot\":%u,\"up\":%lu,\"ep\":%lu,"
            "\"type\":\"%s\",\"reason\":%u,\"p1\":%u,\"p2\":%u}",
            r->seq, r->boot_id, (unsigned long)r->uptime_sec,
            (unsigned long)r->epoch_sec, evt_type_str((evt_type_t)r->type),
            r->reason, r->p1, r->p2);
        if (pos > cap - 200) break;
    }
    pos += snprintf(buf + pos, cap - pos, "]}");
    free(snap);
    return buf;
}

/* =====================================================================
 * Public: activity
 * ===================================================================== */
void evt_mark_ws_rx(void)          { s_last_ws_rx_sec     = now_sec(); }
void evt_mark_backend_request(void){ s_last_backend_sec   = now_sec();
                                     s_last_ws_rx_sec     = now_sec(); }
void evt_mark_heartbeat(void)     { s_last_heartbeat_sec = now_sec();
                                     s_last_ws_rx_sec     = now_sec(); }

uint32_t evt_sec_since_ws_rx(void)
{
    uint32_t last = s_last_ws_rx_sec;
    return last ? (now_sec() - last) : UINT32_MAX;
}
uint32_t evt_sec_since_backend_request(void)
{
    uint32_t last = s_last_backend_sec;
    return last ? (now_sec() - last) : UINT32_MAX;
}
uint32_t evt_sec_since_heartbeat(void)
{
    uint32_t last = s_last_heartbeat_sec;
    return last ? (now_sec() - last) : UINT32_MAX;
}

uint16_t evt_boot_id(void) { return s_boot_id; }




