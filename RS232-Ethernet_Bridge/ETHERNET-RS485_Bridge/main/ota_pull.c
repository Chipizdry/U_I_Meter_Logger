




#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>

#include "websocket_client.h"

#define OTA_PULL_BUF_SIZE 4096

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
}

static esp_err_t process_stream(uint8_t *data, int len)
{
    uint8_t *ptr = data;
    int remaining = len;

    while (remaining > 0) {

        // ================= HEADER (DATA_LEN) =================
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
                    return ESP_FAIL;
                }

                ESP_ERROR_CHECK(
                    esp_partition_erase_range(
                        data_partition,
                        0,
                        data_partition->size
                    )
                );

                header_bytes = 0;
                phase = PULL_PHASE_DATA;
            }
        }

        // ================= DATA =================
        else if (phase == PULL_PHASE_DATA) {

            uint32_t to_write = data_total_len - data_written;
            if (remaining < to_write)
                to_write = remaining;

            if (to_write > 0) {

                ESP_ERROR_CHECK(
                    esp_partition_write(
                        data_partition,
                        data_written,
                        ptr,
                        to_write
                    )
                );

                data_written += to_write;
                ptr += to_write;
                remaining -= to_write;

                ESP_LOGI(TAG, "DATA %lu / %lu", data_written, data_total_len);
                ota_send_progress("Загрузка системных файлов", data_written, data_total_len);
            }

            if (data_written == data_total_len) {
                ESP_LOGW(TAG, "DATA phase complete");
                phase = PULL_PHASE_FW_LEN;
            }
        }

        // ================= FW_LEN =================
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
                    websocket_send_text("{\"ota_phase\":\"Ошибка обновления\",\"progress\":0}" );
                    return ESP_FAIL;
                }

                ESP_ERROR_CHECK(
                    esp_ota_begin(ota_partition, fw_total_len, &ota_handle)
                );

                header_bytes = 0;
                phase = PULL_PHASE_FW;
            }
        }

        // ================= FW =================
        else if (phase == PULL_PHASE_FW) {

            uint32_t to_write = fw_total_len - fw_written;
            if (remaining < to_write)
                to_write = remaining;

            if (to_write > 0) {

                ESP_ERROR_CHECK(
                    esp_ota_write(ota_handle, ptr, to_write)
                );

                fw_written += to_write;
                ptr += to_write;
                remaining -= to_write;

                ESP_LOGI(TAG, "FW %lu / %lu", fw_written, fw_total_len);
                ota_send_progress("Загрузка прошивки", fw_written, fw_total_len);
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
    websocket_send_text("{\"ota_phase\":\"Подготовка к обновлению\",\"progress\":0}" );
    ota_pull_cleanup();

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = OTA_PULL_BUF_SIZE,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = (const char *)ca_cert_pem_start
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_FAIL;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int total_size = esp_http_client_fetch_headers(client);

    ESP_LOGW(TAG, "HTTP content length: %d", total_size);
        if (total_size <= 0) {
            ESP_LOGE(TAG, "Invalid content length");
        }

    uint8_t buffer[OTA_PULL_BUF_SIZE];
    int read_len;

    while (1)
    {
        read_len = esp_http_client_read(client,
                                        (char*)buffer,
                                        OTA_PULL_BUF_SIZE);

        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error");
             websocket_send_text("{\"ota_phase\":\"Ошибка загрузки\",\"progress\":0}" );
            break;
        }

        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGW(TAG, "HTTP transfer complete");
                break;
            }
            continue;
        }

        if (process_stream(buffer, read_len) != ESP_OK) {
            ESP_LOGE(TAG, "Stream processing failed");
            websocket_send_text("{\"ota_phase\":\"Ошибка обработки данных\",\"progress\":0}" );
            ota_pull_cleanup();
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        if (phase == PULL_PHASE_DONE)
            break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (phase != PULL_PHASE_DONE) {
        ESP_LOGE(TAG, "Incomplete firmware");
        websocket_send_text("{\"ota_phase\":\"Неполная загрузка\",\"progress\":0}" );
        ota_pull_cleanup();
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Finalizing OTA...");

    ESP_ERROR_CHECK(esp_ota_end(ota_handle));
    ESP_ERROR_CHECK(esp_ota_set_boot_partition(ota_partition));
    websocket_send_text("{\"ota_phase\":\"Завершение обновления\",\"progress\":100}" );
    ESP_LOGW(TAG, "OTA SUCCESS — rebooting");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

void ota_task(void *param)
{
    char *url = (char*)param;

    ota_pull_start(url);

    free(url);
    vTaskDelete(NULL);
}


static void ota_send_progress(const char *phase_name, uint32_t done, uint32_t total)
{
    if (!ws_connected || total == 0) return;

    char msg[128];
    int percent = (int)((done * 100) / total);
    if (percent > 100) percent = 100;

    snprintf(msg, sizeof(msg), "{\"ota_phase\":\"%s\",\"progress\":%d}", phase_name, percent);

    websocket_send_text(msg);
}