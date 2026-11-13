



#include "system_watchdog.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "websocket_client.h"
#include "network_state.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "pairing_manager.h"
#include "nvs_settings.h"

static const char *TAG = "WDT";

/* ---------------- Параметры задачи ---------------- */
#define WDT_TASK_STACK          5120
#define WDT_TASK_PRIO           4
#define WDT_PERIOD_MS           15000

/* ---------------- Пороги срабатывания (мс) ---------------- */
#define WS_OFFLINE_SOFT_RESTART_MS   (2  * 60 * 1000)    // 2 минуты  → soft restart
#define WS_OFFLINE_REBOOT_MS         (5  * 60 * 1000)    // 5 минут   → reboot
#define WS_STUCK_REBOOT_MS           (3  * 60 * 1000)    // 3 минуты  → reboot (connected, но тишина)
#define NO_NETWORK_REBOOT_MS         (10 * 60 * 1000)    // 10 минут  → reboot
#define HEAP_LOW_REBOOT_MS           (3  * 60 * 1000)    // 3 минуты  → reboot

/* ---------------- Пороги по памяти ---------------- */
#define HEAP_FREE_MIN_THRESHOLD      (15 * 1024)          // 15 KB
#define HEAP_LARGEST_MIN_THRESHOLD   (4  * 1024)          // 4 KB

/* ---------------- NVS для хранения контекста сбоя ---------------- */
#define NVS_NS        "wdt_crash"
#define NVS_KEY       "info"
#define NVS_MAGIC     0x57445421UL    // "WDT!"

typedef struct {
    uint32_t magic;
    uint8_t  reason;              // wdt_reason_t
    uint32_t uptime_sec;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_largest;
    uint8_t  eth_connected;
    uint8_t  wifi_connected;
    uint8_t  ws_connected;
    uint8_t  reserved;
    uint32_t build_number;
    char     build_date[32];
} wdt_crash_info_t;

/* ---------------- Внешние символы ---------------- */
extern TickType_t last_ws_event_tick;
extern user_settings_t user_cfg;
extern fs_session_t    fs_session;
extern system_settings_t sys;

/* ---------------- Состояние ---------------- */
static bool    s_started           = false;
static int64_t s_ws_down_since     = 0;
static int64_t s_no_net_since      = 0;
static int64_t s_no_def_netif_since= 0;
static int64_t s_heap_low_since    = 0;
static bool    s_soft_restart_done = false;

/* =====================================================================
 * Helpers
 * ===================================================================== */

const char *watchdog_reason_str(wdt_reason_t r)
{
    switch (r) {
        case WDT_REASON_NONE:              return "NONE";
        case WDT_REASON_WS_OFFLINE:        return "WS_OFFLINE";
        case WDT_REASON_WS_STUCK:          return "WS_STUCK";
        case WDT_REASON_NO_NETWORK:        return "NO_NETWORK";
        case WDT_REASON_HEAP_LOW:          return "HEAP_LOW";
        case WDT_REASON_NO_DEFAULT_NETIF:  return "NO_DEFAULT_NETIF";
        case WDT_REASON_MANUAL:            return "MANUAL";
        default:                           return "UNKNOWN";
    }
}

static const char *esp_reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:                return "UNKNOWN";
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Активно ли OTA/pairing — в этих режимах watchdog не трогает систему. */
static bool system_busy(void)
{
    if (pairing_is_active()) return true;
    if (!websocket_is_reconnect_enabled()) return true;  // OTA / test_account
    return false;
}

static bool heap_is_low(void)
{
    size_t free_   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return (free_ < HEAP_FREE_MIN_THRESHOLD) || (largest < HEAP_LARGEST_MIN_THRESHOLD);
}

/* Полный лог состояния системы. */
static void log_diagnostics(void)
{
    size_t free_   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_    = esp_get_minimum_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    net_state_t wifi = network_get_wifi_state();
    net_state_t eth  = network_get_ethernet_state();

    TickType_t now = xTaskGetTickCount();
    uint32_t ws_silence_s = 0;
    if (ws_connected && last_ws_event_tick != 0) {
        ws_silence_s = (uint32_t)((now - last_ws_event_tick) * portTICK_PERIOD_MS / 1000);
    }

    esp_netif_t *def = esp_netif_get_default_netif();
    const char *def_name = "none";
    if (def) {
        def_name = esp_netif_get_ifkey(def);
        if (!def_name) def_name = "?";
    }

    ESP_LOGI(TAG,
             "DIAG heap=%u/%u/%u eth=%s wifi=%s ws=%s silence=%us defnetif=%s uptime=%us",
             (unsigned)free_, (unsigned)min_, (unsigned)largest,
             net_state_to_str(eth), net_state_to_str(wifi),
             ws_connected ? "up" : "down",
             (unsigned)ws_silence_s,
             def_name,
             (unsigned)(now_ms() / 1000));
}

/* =====================================================================
 * NVS: сохранение и чтение контекста сбоя
 * ===================================================================== */

static void save_crash_info(wdt_reason_t reason)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS to save crash info");
        return;
    }

    wdt_crash_info_t info = {0};
    info.magic          = NVS_MAGIC;
    info.reason         = (uint8_t)reason;
    info.uptime_sec     = (uint32_t)(now_ms() / 1000);
    info.heap_free      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    info.heap_min       = (uint32_t)esp_get_minimum_free_heap_size();
    info.heap_largest   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    info.eth_connected  = ethernet_is_connected() ? 1 : 0;
    info.wifi_connected = (network_get_wifi_state() == NET_STATE_WIFI_UP) ? 1 : 0;
    info.ws_connected   = ws_connected ? 1 : 0;
    info.build_number   = sys.build_number;
    strncpy(info.build_date, sys.build_date, sizeof(info.build_date) - 1);

    esp_err_t err = nvs_set_blob(h, NVS_KEY, &info, sizeof(info));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Crash info saved: reason=%s uptime=%lus heap=%lu build=%lu",
                 watchdog_reason_str(reason),
                 (unsigned long)info.uptime_sec,
                 (unsigned long)info.heap_free,
                 (unsigned long)info.build_number);
    } else {
        ESP_LOGE(TAG, "Failed to save crash info: %s", esp_err_to_name(err));
    }
}

void watchdog_dump_last_crash(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No previous crash record in NVS");
        return;
    }

    wdt_crash_info_t info = {0};
    size_t size = sizeof(info);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &info, &size);
    nvs_close(h);

    if (err != ESP_OK || info.magic != NVS_MAGIC) {
        ESP_LOGI(TAG, "No valid crash record found");
        return;
    }

    esp_reset_reason_t r = esp_reset_reason();

    ESP_LOGW(TAG, "============ PREVIOUS CRASH CONTEXT ============");
    ESP_LOGW(TAG, "ESP reset reason : %d (%s)", (int)r, esp_reset_reason_str(r));
    ESP_LOGW(TAG, "WDT reason       : %s", watchdog_reason_str((wdt_reason_t)info.reason));
    ESP_LOGW(TAG, "Uptime before    : %lu s", (unsigned long)info.uptime_sec);
    ESP_LOGW(TAG, "Heap free/min/largest : %lu / %lu / %lu",
             (unsigned long)info.heap_free,
             (unsigned long)info.heap_min,
             (unsigned long)info.heap_largest);
    ESP_LOGW(TAG, "ETH connected    : %u", info.eth_connected);
    ESP_LOGW(TAG, "Wi-Fi connected  : %u", info.wifi_connected);
    ESP_LOGW(TAG, "WS connected     : %u", info.ws_connected);
    ESP_LOGW(TAG, "Build            : %lu (%s)",
             (unsigned long)info.build_number, info.build_date);
    ESP_LOGW(TAG, "================================================");

    /* Удаляем запись — чтобы не путать со следующим сбоем. */
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* =====================================================================
 * Действие при сбое
 * ===================================================================== */

static void trigger_reboot(wdt_reason_t reason)
{
    ESP_LOGE(TAG, ">>> WATCHDOG REBOOT: %s <<<", watchdog_reason_str(reason));
    save_crash_info(reason);

    /* Дать UART-логу выйти в порт и сообщению — уйти в WS (если ещё жив). */
    if (ws_connected) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"command_type\":\"watchdog_reboot\",\"reason\":\"%s\"}",
                 watchdog_reason_str(reason));
        websocket_send_text(msg);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* =====================================================================
 * Основная задача
 * ===================================================================== */

static void watchdog_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Watchdog supervisor running");

    /* Ждём стабилизации сети и WS после старта */
    vTaskDelay(pdMS_TO_TICKS(60000));

    s_ws_down_since      = 0;
    s_no_net_since       = 0;
    s_no_def_netif_since = 0;
    s_heap_low_since     = 0;
    s_soft_restart_done  = false;

    uint32_t last_diag_sec = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WDT_PERIOD_MS));

        int64_t now = now_ms();
        uint32_t uptime_sec = (uint32_t)(now / 1000);

        /* Полный DIAG раз в 5 минут */
        if (uptime_sec - last_diag_sec >= 300) {
            last_diag_sec = uptime_sec;
            log_diagnostics();
        }

        /* Не трогаем систему в режимах pairing / OTA / test_account */
        if (system_busy()) {
            s_ws_down_since      = 0;
            s_no_net_since       = 0;
            s_no_def_netif_since = 0;
            s_heap_low_since     = 0;
            s_soft_restart_done  = false;
            continue;
        }

        /* -------- WS -------- */
        if (!ws_connected) {
            if (s_ws_down_since == 0) {
                s_ws_down_since = now;
                ESP_LOGW(TAG, "WS went offline");
            }
            int64_t down_ms = now - s_ws_down_since;

            /* Уровень 1: soft restart WS после 2 мин */
            if (down_ms > WS_OFFLINE_SOFT_RESTART_MS && !s_soft_restart_done) {
                ESP_LOGW(TAG, "WS offline %llds → soft restart websocket",
                         (long long)(down_ms / 1000));
                s_soft_restart_done = true;
                websocket_restart(
                    user_cfg.account_login,
                    fs_session.session_token,
                    user_cfg.account_password,
                    user_cfg.node_name);
            }

            /* Уровень 2: reboot после 5 мин */
            if (down_ms > WS_OFFLINE_REBOOT_MS) {
                trigger_reboot(WDT_REASON_WS_OFFLINE);
            }
        } else {
            s_ws_down_since = 0;
            s_soft_restart_done = false;

            /* WS «висит» — connected, но давно не было событий */
            TickType_t now_t = xTaskGetTickCount();
            if (last_ws_event_tick != 0) {
                uint32_t silence_ms =
                    (uint32_t)((now_t - last_ws_event_tick) * portTICK_PERIOD_MS);
                if (silence_ms > WS_STUCK_REBOOT_MS) {
                    ESP_LOGE(TAG, "WS stuck: no events for %lus",
                             (unsigned long)(silence_ms / 1000));
                    trigger_reboot(WDT_REASON_WS_STUCK);
                }
            }
        }

        /* -------- Сеть -------- */
        if (!network_is_ready()) {
            if (s_no_net_since == 0) {
                s_no_net_since = now;
                ESP_LOGW(TAG, "Network went down (eth+wifi)");
            }
            if ((now - s_no_net_since) > NO_NETWORK_REBOOT_MS) {
                trigger_reboot(WDT_REASON_NO_NETWORK);
            }
        } else {
            s_no_net_since = 0;

            /* Если сеть UP, но default netif не назначен — подозрительно */
            if (esp_netif_get_default_netif() == NULL) {
                if (s_no_def_netif_since == 0) {
                    s_no_def_netif_since = now;
                    ESP_LOGW(TAG, "Network UP but default netif is NULL");
                }
                if ((now - s_no_def_netif_since) > WS_STUCK_REBOOT_MS) {
                    trigger_reboot(WDT_REASON_NO_DEFAULT_NETIF);
                }
            } else {
                s_no_def_netif_since = 0;
            }
        }

        /* -------- Память -------- */
        if (heap_is_low()) {
            if (s_heap_low_since == 0) {
                s_heap_low_since = now;
                ESP_LOGW(TAG, "Heap low: free=%u largest=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            }
            if ((now - s_heap_low_since) > HEAP_LOW_REBOOT_MS) {
                trigger_reboot(WDT_REASON_HEAP_LOW);
            }
        } else {
            s_heap_low_since = 0;
        }
    }
}

/* =====================================================================
 * Public API
 * ===================================================================== */

esp_err_t watchdog_start(void)
{
    if (s_started) return ESP_OK;

    ESP_LOGW(TAG, "ESP reset reason at boot: %d (%s)",
             (int)esp_reset_reason(),
             esp_reset_reason_str(esp_reset_reason()));

    /* Прочитать сохранённый контекст последнего сбоя */
    watchdog_dump_last_crash();

    BaseType_t ret = xTaskCreate(
        watchdog_task,
        "wdt_supervisor",
        WDT_TASK_STACK,
        NULL,
        WDT_TASK_PRIO,
        NULL);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create watchdog task");
        return ESP_FAIL;
    }
    s_started = true;
    ESP_LOGI(TAG, "Watchdog started (period=%d ms)", WDT_PERIOD_MS);
    return ESP_OK;
}

// =====================================================================
// Таблица условий и действий для watchdog
// =====================================================================
// Условие	Порог	Действие
// WS offline	2 мин	soft restart клиента
// WS offline	5 мин	reboot + запись в NVS
// WS connected, но нет событий	3 мин	reboot + запись в NVS
// Нет ни Ethernet, ни Wi-Fi	10 мин	reboot + запись в NVS
// Default netif отсутствует, хотя сеть UP	3 мин	reboot + запись в NVS
// Свободная heap < 15 KB или largest < 4 KB	3 мин	reboot + запись в NVS
// Любая задача зависла (idle не крутится)	30 сек	аппаратный TWDT → panic