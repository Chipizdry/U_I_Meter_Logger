#include "traceroute.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "TRACEROUTE";


/* =========================================================
 * ICMP types / codes
 * ========================================================= */

#define ICMP_DEST_UNREACH       3
#define ICMP_TIME_EXCEEDED      11

#define ICMP_CODE_PORT_UNREACH  3


/* =========================================================
 * UDP base port (как в Linux traceroute)
 * ========================================================= */

#define TRACEROUTE_BASE_PORT    33434

#define TRACEROUTE_MAX_CONSECUTIVE_TIMEOUTS  3


/* =========================================================
 * Task arguments
 * ========================================================= */

typedef struct
{
    char target_host[128];
    traceroute_callback_t callback;
    void *user_ctx;
} traceroute_task_arg_t;


/* =========================================================
 * State
 * ========================================================= */

static volatile bool traceroute_running        = false;
static volatile bool traceroute_stop_requested = false;


/* =========================================================
 * Матчер ICMP-ошибки
 *
 * Пришла ICMP-ошибка на наш UDP probe?
 *   - Time Exceeded       → промежуточный роутер
 *   - Port Unreachable    → достигли целевого узла
 * ========================================================= */

static bool icmp_packet_matches(
    const uint8_t *recv_buf,
    int bytes,
    uint16_t expected_dst_port,
    bool *destination
)
{
    if (!recv_buf || bytes < 28 || !destination)
        return false;

    *destination = false;

    /* --- OUTER IPv4 --- */
    uint8_t outer_ihl     = recv_buf[0] & 0x0F;
    uint8_t outer_proto   = recv_buf[9];
    uint8_t outer_hdr_len = outer_ihl * 4;

    if ((recv_buf[0] >> 4) != 4)        return false;
    if (outer_hdr_len < 20)             return false;
    if (outer_proto != IPPROTO_ICMP)    return false;
    if (bytes < outer_hdr_len + 8)      return false;

    /* --- OUTER ICMP --- */
    const uint8_t *icmp = recv_buf + outer_hdr_len;
    uint8_t type = icmp[0];
    uint8_t code = icmp[1];

    if (type != ICMP_TIME_EXCEEDED &&
        type != ICMP_DEST_UNREACH)
        return false;

    /* --- INNER IPv4 --- */
    const uint8_t *inner_ip = recv_buf + outer_hdr_len + 8;
    int remaining = bytes - outer_hdr_len - 8;
    if (remaining < 20) return false;

    uint8_t inner_ihl     = inner_ip[0] & 0x0F;
    uint8_t inner_proto   = inner_ip[9];
    uint8_t inner_hdr_len = inner_ihl * 4;

    if ((inner_ip[0] >> 4) != 4)       return false;
    if (inner_hdr_len < 20)            return false;
    if (inner_proto != IPPROTO_UDP)    return false;
    if (remaining < inner_hdr_len + 8) return false;

    /* --- INNER UDP --- */
    const uint8_t *inner_udp = inner_ip + inner_hdr_len;
    uint16_t inner_dst_port  =
        ((uint16_t)inner_udp[2] << 8) | inner_udp[3];

    if (inner_dst_port != expected_dst_port)
        return false;

    if (type == ICMP_DEST_UNREACH && code == ICMP_CODE_PORT_UNREACH)
        *destination = true;

    return true;
}


/* =========================================================
 * Task
 * ========================================================= */

static void traceroute_task(void *pvParameter)
{
    traceroute_task_arg_t *arg = (traceroute_task_arg_t *)pvParameter;

    traceroute_result_t result;
    memset(&result, 0, sizeof(result));

    strlcpy(result.host, arg->target_host, sizeof(result.host));
    result.max_hops = TRACEROUTE_MAX_HOPS;
    result.error    = ESP_OK;

    ESP_LOGI(TAG, "Starting UDP traceroute to %s", arg->target_host);

    /* --- DNS --- */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    int dns_result = getaddrinfo(arg->target_host, NULL, &hints, &res);
    if (dns_result != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS resolution failed for %s", arg->target_host);
        result.error = ESP_ERR_NOT_FOUND;
        goto finished;
    }

    struct sockaddr_in target_addr;
    memcpy(&target_addr, res->ai_addr, sizeof(target_addr));
    freeaddrinfo(res);

    inet_ntop(AF_INET, &target_addr.sin_addr,
              result.destination_ip, sizeof(result.destination_ip));

    ESP_LOGI(TAG, "Target: %s [%s]",
             arg->target_host, result.destination_ip);

    /* --- UDP socket для отправки --- */
    int udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket() failed: errno=%d", errno);
        result.error = ESP_FAIL;
        goto finished;
    }

    /* --- RAW ICMP socket для приёма ошибок --- */
    int icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (icmp_sock < 0) {
        ESP_LOGE(TAG, "ICMP socket() failed: errno=%d", errno);
        close(udp_sock);
        result.error = ESP_FAIL;
        goto finished;
    }

    /* --- Таймаут приёма --- */
    struct timeval tv;
    tv.tv_sec  = TRACEROUTE_TIMEOUT_MS / 1000;
    tv.tv_usec = (TRACEROUTE_TIMEOUT_MS % 1000) * 1000;
    if (setsockopt(icmp_sock, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, sizeof(tv)) < 0) {
        ESP_LOGW(TAG, "SO_RCVTIMEO failed: errno=%d", errno);
    }

    uint8_t udp_payload[32];
    memset(udp_payload, 'A', sizeof(udp_payload));

    uint8_t recv_buf[512];

    int consecutive_timeouts = 0;

    for (int ttl = 1; ttl <= TRACEROUTE_MAX_HOPS; ttl++) {

        if (traceroute_stop_requested) break;

        traceroute_hop_t *hop = &result.hops[result.hop_count];
        memset(hop, 0, sizeof(*hop));
        hop->hop         = ttl;
        hop->responses   = 0;
        hop->timeout     = true;
        hop->destination = false;

        for (int i = 0; i < TRACEROUTE_PROBES_PER_HOP; i++)
            hop->rtt_ms[i] = -1.0f;

        /* --- Устанавливаем TTL на UDP-сокете --- */
        if (setsockopt(udp_sock, IPPROTO_IP, IP_TTL,
                       &ttl, sizeof(ttl)) < 0) {
            ESP_LOGE(TAG, "IP_TTL failed: errno=%d", errno);
            result.error = ESP_FAIL;
            break;
        }

        for (int probe = 0; probe < TRACEROUTE_PROBES_PER_HOP; probe++) {

            if (traceroute_stop_requested) break;

            /* Уникальный dst port на каждый probe */
            uint16_t dst_port = (uint16_t)(
                TRACEROUTE_BASE_PORT +
                (ttl - 1) * TRACEROUTE_PROBES_PER_HOP +
                probe
            );

            struct sockaddr_in dst = target_addr;
            dst.sin_port = htons(dst_port);

            int64_t start = esp_timer_get_time();

            int sent = sendto(udp_sock,
                              udp_payload, sizeof(udp_payload), 0,
                              (struct sockaddr *)&dst, sizeof(dst));

            if (sent < 0) {
                ESP_LOGE(TAG,
                    "UDP sendto failed TTL=%d probe=%d errno=%d (%s)",
                    ttl, probe + 1, errno, strerror(errno));
                result.error = ESP_FAIL;
                break;
            }

            bool destination = false;
            bool found       = false;
            char router_ip[INET_ADDRSTRLEN] = {0};
            float rtt_ms = -1.0f;

            while (!traceroute_stop_requested) {

                int64_t elapsed = esp_timer_get_time() - start;
                if (elapsed >= ((int64_t)TRACEROUTE_TIMEOUT_MS * 1000))
                    break;

                struct sockaddr_in sender_addr;
                socklen_t addr_len = sizeof(sender_addr);

                int bytes = recvfrom(icmp_sock,
                                     recv_buf, sizeof(recv_buf), 0,
                                     (struct sockaddr *)&sender_addr,
                                     &addr_len);

                if (bytes < 0) {
                    /* EAGAIN = штатный таймаут */
                    break;
                }

                if (!icmp_packet_matches(recv_buf, bytes,
                                         dst_port, &destination))
                    continue;

                int64_t end = esp_timer_get_time();
                rtt_ms = (float)(end - start) / 1000.0f;

                inet_ntop(AF_INET, &sender_addr.sin_addr,
                          router_ip, sizeof(router_ip));

                found = true;
                break;
            }

            if (found) {
                hop->rtt_ms[probe] = roundf(rtt_ms * 100.0f) / 100.0f;
                hop->responses++;
                hop->timeout     = false;
                hop->destination |= destination;

                if (hop->ip[0] == '\0')
                    strlcpy(hop->ip, router_ip, sizeof(hop->ip));

                ESP_LOGI(TAG, "%2d  probe=%d  %7.2f ms  %s%s",
                         ttl, probe + 1, rtt_ms, router_ip,
                         destination ? " [TARGET]" : "");
            } else {
                ESP_LOGI(TAG, "%2d  probe=%d  *", ttl, probe + 1);
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        result.hop_count++;

        ESP_LOGI(TAG, "HOP %d: %s  %0.2f / %0.2f / %0.2f ms",
                 ttl, hop->ip[0] ? hop->ip : "*",
                 hop->rtt_ms[0], hop->rtt_ms[1], hop->rtt_ms[2]);

        /* --- Early termination --- */
        if (hop->timeout)
            consecutive_timeouts++;
        else
            consecutive_timeouts = 0;

        if (consecutive_timeouts >= TRACEROUTE_MAX_CONSECUTIVE_TIMEOUTS) {
            ESP_LOGW(TAG, "%d пустых хопов подряд — прекращаем",
                     consecutive_timeouts);
            break;
        }

        if (hop->destination) {
            result.reached = true;
            ESP_LOGI(TAG, "Target reached");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    close(icmp_sock);
    close(udp_sock);

finished:

    if (result.error != ESP_OK)
        result.reached = false;

    ESP_LOGI(TAG, "Traceroute finished: hops=%d reached=%s error=%s",
             result.hop_count, result.reached ? "yes" : "no",
             esp_err_to_name(result.error));

    if (arg->callback)
        arg->callback(&result, arg->user_ctx);

    traceroute_running        = false;
    traceroute_stop_requested = false;

    free(arg);
    vTaskDelete(NULL);
}


/* =========================================================
 * Public API
 * ========================================================= */

esp_err_t traceroute_start(
    const char *target_host,
    traceroute_callback_t callback,
    void *user_ctx)
{
    if (!target_host || target_host[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    if (!callback)
        return ESP_ERR_INVALID_ARG;
    if (traceroute_running)
        return ESP_ERR_INVALID_STATE;

    traceroute_task_arg_t *arg =
        calloc(1, sizeof(traceroute_task_arg_t));
    if (!arg)
        return ESP_ERR_NO_MEM;

    strlcpy(arg->target_host, target_host, sizeof(arg->target_host));
    arg->callback = callback;
    arg->user_ctx = user_ctx;

    traceroute_stop_requested = false;
    traceroute_running        = true;

    BaseType_t ret = xTaskCreate(traceroute_task, "traceroute",
                                  6144, arg, 4, NULL);
    if (ret != pdPASS) {
        traceroute_running = false;
        free(arg);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}


bool traceroute_is_running(void) { return traceroute_running; }

void traceroute_stop(void)
{
    if (traceroute_running)
        traceroute_stop_requested = true;
}


