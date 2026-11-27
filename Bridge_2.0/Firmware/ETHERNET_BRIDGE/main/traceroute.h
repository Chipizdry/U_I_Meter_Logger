#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* =========================================================
 * Configuration
 * ========================================================= */

#define TRACEROUTE_MAX_HOPS          30
#define TRACEROUTE_PROBES_PER_HOP    3

/*
 * Timeout одного ICMP запроса.
 *
 * 1000 ms × 3 probe × 30 hops = максимум около 90 секунд.
 *
 * Это выполняется в отдельной FreeRTOS task,
 * поэтому остальные задачи ESP32 не блокируются.
 */
#define TRACEROUTE_TIMEOUT_MS        1000

#define TRACEROUTE_PACKET_SIZE       32


/* =========================================================
 * One hop
 * ========================================================= */

typedef struct
{
    int hop;

    char ip[16];

    /*
     * RTT каждого из трёх запросов.
     *
     * Если timeout:
     *     rtt_ms[x] = -1.0f
     */
    float rtt_ms[TRACEROUTE_PROBES_PER_HOP];

    /*
     * Сколько ответов реально получено.
     */
    uint8_t responses;

    /*
     * true если все три запроса
     * получили timeout.
     */
    bool timeout;

    /*
     * true если этот hop является
     * конечным узлом.
     */
    bool destination;

} traceroute_hop_t;


/* =========================================================
 * Complete traceroute result
 * ========================================================= */

typedef struct
{
    /*
     * Исходное имя/адрес,
     * например:
     *
     * mts.beaver.lb.cor-int.com
     */
    char host[128];

    /*
     * IP конечного узла.
     */
    char destination_ip[16];

    /*
     * Максимальное количество hop.
     */
    int max_hops;

    /*
     * Количество реально пройденных hop.
     */
    int hop_count;

    /*
     * true если конечный узел достигнут.
     */
    bool reached;

    /*
     * ESP_OK если traceroute завершён нормально.
     *
     * При ошибке здесь будет esp_err_t.
     */
    esp_err_t error;

    /*
     * Результаты всех hop.
     */
    traceroute_hop_t hops[TRACEROUTE_MAX_HOPS];

} traceroute_result_t;


/* =========================================================
 * Callback
 * ========================================================= */

typedef void (*traceroute_callback_t)(
    const traceroute_result_t *result,
    void *user_ctx
);


/* =========================================================
 * API
 * ========================================================= */

esp_err_t traceroute_start(
    const char *target_host,
    traceroute_callback_t callback,
    void *user_ctx
);


bool traceroute_is_running(void);


void traceroute_stop(void);


#ifdef __cplusplus
}
#endif


