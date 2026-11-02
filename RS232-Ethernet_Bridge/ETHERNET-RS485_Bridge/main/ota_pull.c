#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "websocket_client.h"

#define OTA_PULL_BUF_SIZE 1024
#define FLASH_SECTOR_SIZE 4096

static const char *TAG = "OTA_PULL";

extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

typedef enum {
    PULL_PHASE_HEADER = 0,
    PULL_PHASE_DATA,
    PULL_PHASE_FW_LEN,
    PULL_PHASE_FW,
    PULL_PHASE_DONE
} pull_phase_t;

static void ota_send_progress(const char *phase_name, uint32_t done, uint32_t total);
static void ota_send_storage_info(const char *label,const esp_partition_t *part,uint32_t used);

static pull_phase_t phase = PULL_PHASE_HEADER;

static uint32_t data_total_len = 0;
static uint32_t data_written   = 0;

static uint32_t fw_total_len = 0;
static uint32_t fw_written   = 0;

static const esp_partition_t *data_partition = NULL;
static const esp_partition_t *ota_partition  = NULL;
static esp_ota_handle_t ota_handle = 0;

static uint8_t header_buf[4];
static int header_bytes = 0;

/* Троттлинг прогресса/логов (НЕ чанков!) */
static uint32_t last_pct_data = 0xFFFFFFFF;
static uint32_t last_pct_fw   = 0xFFFFFFFF;

/* Буфер чтения HTTP — вынесен из стека задачи в BSS. */
static uint8_t s_ota_buf[OTA_PULL_BUF_SIZE];

static void ota_pull_cleanup(void)
{
    if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
    phase = PULL_PHASE_HEADER;
    data_total_len = 0;
    data_written = 0;
    fw_total_len = 0;
    fw_written = 0;
    header_bytes = 0;
    last_pct_data = 0xFFFFFFFF;
    last_pct_fw   = 0xFFFFFFFF;
}


/* Строковое имя подтипа раздела (для отображения клиенту) */
static const char *part_subtype_str(const esp_partition_t *part)
{
    if (!part) return "null";

    if (part->type == ESP_PARTITION_TYPE_APP) {
        switch (part->subtype) {
            case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
            case ESP_PARTITION_SUBTYPE_APP_OTA_0:   return "ota_0";
            case ESP_PARTITION_SUBTYPE_APP_OTA_1:   return "ota_1";
            case ESP_PARTITION_SUBTYPE_APP_OTA_2:   return "ota_2";
            case ESP_PARTITION_SUBTYPE_APP_OTA_3:   return "ota_3";
            case ESP_PARTITION_SUBTYPE_APP_OTA_4:   return "ota_4";
            case ESP_PARTITION_SUBTYPE_APP_OTA_5:   return "ota_5";
            case ESP_PARTITION_SUBTYPE_APP_OTA_6:   return "ota_6";
            case ESP_PARTITION_SUBTYPE_APP_OTA_7:   return "ota_7";
            case ESP_PARTITION_SUBTYPE_APP_OTA_8:   return "ota_8";
            case ESP_PARTITION_SUBTYPE_APP_OTA_9:   return "ota_9";
            case ESP_PARTITION_SUBTYPE_APP_OTA_10:  return "ota_10";
            case ESP_PARTITION_SUBTYPE_APP_OTA_11:  return "ota_11";
            case ESP_PARTITION_SUBTYPE_APP_OTA_12:  return "ota_12";
            case ESP_PARTITION_SUBTYPE_APP_OTA_13:  return "ota_13";
            case ESP_PARTITION_SUBTYPE_APP_OTA_14:  return "ota_14";
            case ESP_PARTITION_SUBTYPE_APP_OTA_15:  return "ota_15";
            case ESP_PARTITION_SUBTYPE_APP_TEST:    return "test";
            default:                                 return "app";
        }
    }

    if (part->type == ESP_PARTITION_TYPE_DATA) {
        switch (part->subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "otadata";
            case ESP_PARTITION_SUBTYPE_DATA_PHY:      return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:      return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:   return "spiffs";
            case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "littlefs";
            default:                                   return "data";
        }
    }

    return "unknown";
}


/* ============================================================
 * NEW: отправка отчёта о размещении раздела
 * ============================================================ */
static void ota_send_storage_info(const char *label,
                                  const esp_partition_t *part,
                                  uint32_t used)
{
    if (!ws_connected) return;

    if (!part) {
        ESP_LOGE(TAG, "ota_send_storage_info: part=NULL");
        websocket_send_text("{\"ota_phase\":\"Раздел не найден\",\"progress\":0}");
        return;
    }

    uint32_t total      = (uint32_t)part->size;
    uint32_t free_bytes = (total > used) ? (total - used) : 0;
    int      pct        = (total > 0)
        ? (int)((uint64_t)used * 100ULL / (uint64_t)total)
        : 0;

    uint32_t heap_free = (uint32_t)esp_get_free_heap_size();
    uint32_t heap_min  = (uint32_t)esp_get_minimum_free_heap_size();

    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *running_label = running ? running->label : "n/a";
    const char *sub_name = part_subtype_str(part);

    char msg[512];
    snprintf(msg, sizeof(msg),
        "{\"ota_phase\":\"%s\","
        "\"partition\":\"%s\","
        "\"subtype\":\"%s\","
        "\"address\":%" PRIu32 ","
        "\"address_hex\":\"0x%06" PRIx32 "\","
        "\"total\":%" PRIu32 ","
        "\"used\":%" PRIu32 ","
        "\"free\":%" PRIu32 ","
        "\"percent\":%d,"
        "\"running_from\":\"%s\","
        "\"heap_free\":%" PRIu32 ","
        "\"heap_min\":%" PRIu32 "}",
        label,
        part->label,
        sub_name,
        (uint32_t)part->address,
        (uint32_t)part->address,
        total,
        used,
        free_bytes,
        pct,
        running_label,
        heap_free,
        heap_min);

    ESP_LOGI(TAG,
        "STORAGE [%s]: part=\"%s\" (%s) @0x%06lx size=%lu, "
        "used=%lu/%lu (%d%%), free=%lu, running_from=%s, heap=%lu (min %lu)",
        label,
        part->label, sub_name,
        (unsigned long)part->address,
        (unsigned long)total,
        (unsigned long)used, (unsigned long)total, pct,
        (unsigned long)free_bytes,
        running_label,
        (unsigned long)heap_free,
        (unsigned long)heap_min);

    websocket_send_text(msg);
}



static esp_err_t process_stream(uint8_t *data, int len)
{
    uint8_t *ptr = data;
    int remaining = len;

    while (remaining > 0) {

        /* ================= HEADER (DATA_LEN) ================= */
        if (phase == PULL_PHASE_HEADER) {

            int needed = 4 - header_bytes;
            int to_copy = remaining < needed ? remaining : needed;

            memcpy(header_buf + header_bytes, ptr, to_copy);
            header_bytes += to_copy;
            ptr += to_copy;
            remaining -= to_copy;

            if (header_bytes == 4) {
                memcpy(&data_total_len, header_buf, 4);
                ESP_LOGW(TAG, "DATA total size: %lu", data_total_len);

                data_partition = esp_partition_find_first(
                    ESP_PARTITION_TYPE_DATA,
                    ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
                    NULL
                );

                if (!data_partition) {
                    ESP_LOGE(TAG, "LittleFS partition not found");
                    websocket_send_text(
                        "{\"ota_phase\":\"Раздел LittleFS не найден\",\"progress\":0}");
                    return ESP_FAIL;
                }

                /* NEW: сообщаем клиенту, сколько занимает образ
                 *      относительно раздела ДО стирания/записи */
              ota_send_storage_info("Размещение системных файлов", data_partition, data_total_len);

                if (data_total_len > data_partition->size) {
                    ESP_LOGE(TAG, "DATA too large: %lu > %lu",
                             data_total_len, (uint32_t)data_partition->size);
                    websocket_send_text(
                        "{\"ota_phase\":\"Системные файлы не влезают\",\"progress\":0}");
                    return ESP_FAIL;
                }

                /* Стираем только нужный диапазон, а не весь раздел */
                uint32_t erase_size = (data_total_len + FLASH_SECTOR_SIZE - 1)
                                      & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);

                if (erase_size > 0) {
                    esp_err_t err = esp_partition_erase_range(
                        data_partition, 0, erase_size);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "erase_range failed: %s", esp_err_to_name(err));
                        return err;
                    }
                    ESP_LOGI(TAG, "Erased %lu bytes (not whole %lu-byte partition)",
                             erase_size, (uint32_t)data_partition->size);
                }

                header_bytes = 0;
                phase = PULL_PHASE_DATA;
            }
        }

        /* ================= DATA ================= */
        else if (phase == PULL_PHASE_DATA) {

            uint32_t to_write = data_total_len - data_written;
            if ((uint32_t)remaining < to_write)
                to_write = (uint32_t)remaining;

            if (to_write > 0) {
                esp_err_t err = esp_partition_write(
                    data_partition, data_written, ptr, to_write);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "partition_write failed: %s", esp_err_to_name(err));
                    return err;
                }

                data_written += to_write;
                ptr += to_write;
                remaining -= to_write;

                uint32_t pct = (data_total_len > 0)
                    ? (data_written * 100U / data_total_len)
                    : 100;
                if (pct != last_pct_data) {
                    last_pct_data = pct;
                    ESP_LOGI(TAG, "DATA %lu / %lu (%lu%%)",
                             data_written, data_total_len, pct);
                    ota_send_progress("Загрузка системных файлов",
                                      data_written, data_total_len);
                }
            }

            if (data_written == data_total_len) {
                ESP_LOGW(TAG, "DATA phase complete");
                phase = PULL_PHASE_FW_LEN;
            }
        }

        /* ================= FW_LEN ================= */
        else if (phase == PULL_PHASE_FW_LEN) {

            int needed = 4 - header_bytes;
            int to_copy = remaining < needed ? remaining : needed;

            memcpy(header_buf + header_bytes, ptr, to_copy);
            header_bytes += to_copy;
            ptr += to_copy;
            remaining -= to_copy;

            if (header_bytes == 4) {
                memcpy(&fw_total_len, header_buf, 4);
                ESP_LOGW(TAG, "FW total size: %lu", fw_total_len);

                ota_partition = esp_ota_get_next_update_partition(NULL);
                if (!ota_partition) {
                    ESP_LOGE(TAG, "No OTA partition");
                    websocket_send_text(
                        "{\"ota_phase\":\"Ошибка обновления\",\"progress\":0}");
                    return ESP_FAIL;
                }

                /* NEW: сообщаем клиенту, сколько занимает прошивка
                 *      относительно OTA-слота ДО ota_begin */
                ota_send_storage_info("Размещение прошивки",
                      ota_partition, fw_total_len);

                if (fw_total_len > ota_partition->size) {
                    ESP_LOGE(TAG, "FW too large: %lu > %lu",
                             fw_total_len, (uint32_t)ota_partition->size);
                    websocket_send_text(
                        "{\"ota_phase\":\"Прошивка не влезает в раздел\",\"progress\":0}");
                    return ESP_FAIL;
                }

                esp_err_t err = esp_ota_begin(ota_partition, 0, &ota_handle);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
                    return err;
                }

                header_bytes = 0;
                phase = PULL_PHASE_FW;
            }
        }

        /* ================= FW ================= */
        else if (phase == PULL_PHASE_FW) {

            uint32_t to_write = fw_total_len - fw_written;
            if ((uint32_t)remaining < to_write)
                to_write = (uint32_t)remaining;

            if (to_write > 0) {
                esp_err_t err = esp_ota_write(ota_handle, ptr, to_write);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
                    return err;
                }

                fw_written += to_write;
                ptr += to_write;
                remaining -= to_write;

                uint32_t pct = (fw_total_len > 0)
                    ? (fw_written * 100U / fw_total_len)
                    : 100;
                if (pct != last_pct_fw) {
                    last_pct_fw = pct;
                    ESP_LOGI(TAG, "FW %lu / %lu (%lu%%)",
                             fw_written, fw_total_len, pct);
                    ota_send_progress("Загрузка прошивки",
                                      fw_written, fw_total_len);
                }
            }

            if (fw_written == fw_total_len) {
                ESP_LOGW(TAG, "FW download complete");
                phase = PULL_PHASE_DONE;
                return ESP_OK;
            }
        }
    }

    return ESP_OK;
}

esp_err_t ota_pull_start(const char *url)
{
    ESP_LOGW(TAG, "Starting OTA pull from: %s", url);
    websocket_send_text("{\"ota_phase\":\"Подготовка к обновлению\",\"progress\":0}");
    ota_pull_cleanup();

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = OTA_PULL_BUF_SIZE,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)ca_cert_pem_start,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        websocket_enable_reconnect();
        return ESP_FAIL;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed");
        esp_http_client_cleanup(client);
        websocket_enable_reconnect();
        return ESP_FAIL;
    }

    int total_size = esp_http_client_fetch_headers(client);
    ESP_LOGW(TAG, "HTTP content length: %d", total_size);
    if (total_size <= 0) {
        ESP_LOGE(TAG, "Invalid content length (proceeding chunk-by-chunk)");
    }

    int read_len;

    while (1)
    {
        read_len = esp_http_client_read(client, (char*)s_ota_buf,
                                        OTA_PULL_BUF_SIZE);

        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            websocket_send_text("{\"ota_phase\":\"Ошибка загрузки\",\"progress\":0}");
            break;
        }

        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGW(TAG, "HTTP transfer complete");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (process_stream(s_ota_buf, read_len) != ESP_OK) {
            ESP_LOGE(TAG, "Stream processing failed");
            websocket_send_text(
                "{\"ota_phase\":\"Ошибка обработки данных\",\"progress\":0}");
            ota_pull_cleanup();
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            websocket_enable_reconnect();
            return ESP_FAIL;
        }

        if (phase == PULL_PHASE_DONE)
            break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (phase != PULL_PHASE_DONE) {
        ESP_LOGE(TAG, "Incomplete firmware");
        websocket_send_text("{\"ota_phase\":\"Неполная загрузка\",\"progress\":0}");
        ota_pull_cleanup();
        websocket_enable_reconnect();
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Finalizing OTA...");

    esp_err_t err = esp_ota_end(ota_handle);
    ota_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        websocket_send_text("{\"ota_phase\":\"Ошибка финализации\",\"progress\":0}");
        ota_pull_cleanup();
        websocket_enable_reconnect();
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        websocket_send_text("{\"ota_phase\":\"Ошибка установки загрузки\",\"progress\":0}");
        ota_pull_cleanup();
        websocket_enable_reconnect();
        return ESP_FAIL;
    }

    /* NEW: финальный отчёт по памяти */
    if (ws_connected) {
        char final_msg[160];
        snprintf(final_msg, sizeof(final_msg),
            "{\"ota_phase\":\"OTA успешно завершена\","
            "\"heap_free\":%" PRIu32 ","
            "\"heap_min\":%" PRIu32 "}",
            (uint32_t)esp_get_free_heap_size(),
            (uint32_t)esp_get_minimum_free_heap_size());
        websocket_send_text(final_msg);
    }

    websocket_send_text("{\"ota_phase\":\"Завершение обновления\",\"progress\":100}");
    ESP_LOGW(TAG, "OTA SUCCESS — rebooting");

    vTaskDelay(pdMS_TO_TICKS(1000));
    websocket_send_text("{\"ota_phase\":\"Обновление завершено, перезагрузка\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

void ota_task(void *param)
{
    char *url = (char*)param;

    esp_err_t err = ota_pull_start(url);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA pull failed: %s", esp_err_to_name(err));
        ota_pull_cleanup();
        websocket_enable_reconnect();
    }

    free(url);
    vTaskDelete(NULL);
}

static void ota_send_progress(const char *phase_name, uint32_t done, uint32_t total)
{
    if (!ws_connected || total == 0) return;
    char msg[128];
    int percent = (int)((done * 100U) / total);
    if (percent > 100) percent = 100;
    snprintf(msg, sizeof(msg),
             "{\"ota_phase\":\"%s\",\"progress\":%d}", phase_name, percent);
    websocket_send_text(msg);
}


