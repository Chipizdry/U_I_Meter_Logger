


#pragma once
#include "esp_err.h"

/**
 * Причина, по которой watchdog инициировал перезагрузку.
 * Сохраняется в NVS и логируется при следующем старте.
 */
typedef enum {
    WDT_REASON_NONE = 0,
    WDT_REASON_WS_OFFLINE,        // WS не подключался > 5 минут
    WDT_REASON_WS_STUCK,          // WS connected, но нет событий > 3 минут
    WDT_REASON_NO_NETWORK,        // нет ни Ethernet, ни Wi-Fi > 10 минут
    WDT_REASON_HEAP_LOW,          // критически мало памяти > 3 минут
    WDT_REASON_NO_DEFAULT_NETIF,  // сеть UP, но default netif отсутствует > 3 минут
    WDT_REASON_MANUAL,            // зарезервировано
} wdt_reason_t;

/**
 * Запустить фоновую задачу-супервизор.
 * Вызывать в конце app_main() после network_failover_start().
 */
esp_err_t watchdog_start(void);

/** Строковое представление причины. */
const char *watchdog_reason_str(wdt_reason_t r);

/** Прочитать из NVS и залогировать информацию о последнем сбое. */
void watchdog_dump_last_crash(void);



