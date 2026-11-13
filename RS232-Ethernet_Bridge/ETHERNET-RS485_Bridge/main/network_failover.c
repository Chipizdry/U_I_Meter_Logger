


#include "network_failover.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "network_state.h"

static const char *TAG = "failover";

// Хост для проверки доступности интернета. Можно заменить на ping.
static const char *TEST_HOST = "www.google.com";

// Сколько неудачных проверок подряд считать сбоем
#define FAILURE_THRESHOLD 2
// Интервал между проверками (мс)
#define CHECK_INTERVAL_MS 30000

static int s_failure_count = 0;

/**
 * @brief Проверяет, разрешается ли тестовое доменное имя.
 * @return true если интернет доступен, false в противном случае.
 */
static bool check_connectivity(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    int err = getaddrinfo(TEST_HOST, NULL, &hints, &res);
    if (err == 0) {
        freeaddrinfo(res);
        return true;
    }

 //   ESP_LOGW(TAG, "Проверка связи не удалась: %s", gai_strerror(err));
    return false;
}

/**
 * @brief Возвращает указатель на esp_netif для Ethernet, если он активен.
 */
static esp_netif_t *get_active_eth_netif(void)
{
    if (ethernet_is_connected()) {
        return ethernet_get_netif();
    }
    return NULL;
}

/**
 * @brief Возвращает указатель на esp_netif для Wi-Fi STA, если он активен.
 */
static esp_netif_t *get_active_sta_netif(void)
{
    if (network_get_wifi_state() == NET_STATE_WIFI_UP) {
        return wifi_manager_get_sta_netif();
    }
    return NULL;
}

/**
 * @brief Основная задача: проверка и переключение.
 */
static void failover_task(void *pvParameters)
{
    // Даём системе время на первичное подключение
    vTaskDelay(pdMS_TO_TICKS(10000));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));

        esp_netif_t *current = esp_netif_get_default_netif();
        if (!current) {
            // Если маршрута по умолчанию нет, пытаемся назначить любой активный
            esp_netif_t *eth = get_active_eth_netif();
            esp_netif_t *sta = get_active_sta_netif();
            if (eth) {
                ESP_LOGW(TAG, "Default netif отсутствует. Назначаем Ethernet.");
                esp_netif_set_default_netif(eth);
            } else if (sta) {
                ESP_LOGW(TAG, "Default netif отсутствует. Назначаем Wi-Fi STA.");
                esp_netif_set_default_netif(sta);
            }
            continue;
        }

        if (check_connectivity()) {
            s_failure_count = 0; // Связь есть, сбрасываем счётчик
            continue;
        }

        s_failure_count++;
        if (s_failure_count < FAILURE_THRESHOLD) {
            continue; // Ещё не время для переключения
        }

        // --- Пора переключаться ---
        esp_netif_t *eth = get_active_eth_netif();
        esp_netif_t *sta = get_active_sta_netif();

        if (current == eth && sta) {
            ESP_LOGW(TAG, "Переключение: Ethernet -> Wi-Fi STA");
            esp_netif_set_default_netif(sta);
            s_failure_count = 0;
        } else if (current == sta && eth) {
            ESP_LOGW(TAG, "Переключение: Wi-Fi STA -> Ethernet");
            esp_netif_set_default_netif(eth);
            s_failure_count = 0;
        } else {
            ESP_LOGE(TAG, "Нет альтернативного активного интерфейса для переключения");
        }
    }
}

esp_err_t network_failover_start(void)
{
    BaseType_t ret = xTaskCreate(
        failover_task,
        "net_failover",
        4096,
        NULL,
        5,
        NULL
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу failover");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Мониторинг и переключение интерфейсов запущены");
    return ESP_OK;
}


