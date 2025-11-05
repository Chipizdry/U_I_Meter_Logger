

#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define OTA_CHUNK_SIZE 4096
static const char *TAG = "OTA";

static bool ota_started = false;
static esp_ota_handle_t ota_handle = 0;
static int total_received = 0;
static int total_size = 0;

void ota_init(void) {
    ota_started = false;
    total_received = 0;
    ota_handle = 0;
}

/**
 * OTA POST handler compatible with multipart FormData chunked uploads.
 * Each POST carries a part of the firmware file with fields:
 * - fileName
 * - totalSize
 * - chunkNumber
 * - chunk (binary)
 */
esp_err_t ota_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== OTA POST === len=%d total_received=%d ===", req->content_len, total_received);

    char *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory for OTA buffer");
        return ESP_FAIL;
    }

    int total_read = 0;
    int received = 0;

    // читаем весь запрос полностью
    while (total_read < req->content_len) {
        received = httpd_req_recv(req, buffer + total_read, req->content_len - total_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "Error reading OTA chunk (received=%d, total_read=%d)", received, total_read);
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read error");
            return ESP_FAIL;
        }
        total_read += received;
    }

    received = total_read;
    ESP_LOGI(TAG, "Full POST body read: %d bytes", received);

    // для отладки — показать первые и последние байты пакета
    int preview_len = received < 128 ? received : 128;
    ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, preview_len, ESP_LOG_INFO);
    if (received > 128) {
        ESP_LOGI(TAG, "... (cut) ...");
        ESP_LOG_BUFFER_HEXDUMP(TAG, buffer + received - 64, 64, ESP_LOG_INFO);
    }

    // поиск полей multipart
    char *total_ptr = memmem(buffer, received, "name=\"totalSize\"", strlen("name=\"totalSize\""));
    char *chunk_ptr = memmem(buffer, received, "name=\"chunkNumber\"", strlen("name=\"chunkNumber\""));
    char *file_ptr  = memmem(buffer, received, "application/octet-stream", strlen("application/octet-stream"));

    if (!total_ptr || !chunk_ptr || !file_ptr) {
        ESP_LOGE(TAG, "Multipart fields missing: total=%p chunk=%p file=%p", total_ptr, chunk_ptr, file_ptr);
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid multipart data");
        return ESP_FAIL;
    }

    // извлекаем totalSize
    char total_buf[16] = {0};
    char *p = total_ptr + strlen("name=\"totalSize\"");
    while (*p && !isdigit((int)*p)) p++;
    for (int i = 0; i < sizeof(total_buf) - 1 && isdigit((int)p[i]); i++)
        total_buf[i] = p[i];
    total_size = atoi(total_buf);

    // извлекаем chunkNumber
    char chunk_buf[16] = {0};
    p = chunk_ptr + strlen("name=\"chunkNumber\"");
    while (*p && !isdigit((int)*p)) p++;
    for (int i = 0; i < sizeof(chunk_buf) - 1 && isdigit((int)p[i]); i++)
        chunk_buf[i] = p[i];
    int chunk_number = atoi(chunk_buf);

    ESP_LOGI(TAG, "Parsed multipart fields: chunk=%d, totalSize=%d", chunk_number, total_size);




    // Определяем начало бинарных данных
    char *data_start = strstr(file_ptr, "\r\n\r\n");
    if (!data_start) {
        ESP_LOGE(TAG, "No binary data found in chunk %d", chunk_number);
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No binary data");
        return ESP_FAIL;
    }
    data_start += 4; // пропускаем \r\n\r\n

    int data_len = received - (data_start - buffer);

    // Попробуем найти boundary в конце тела
    const char *boundary_hint = "------WebKitFormBoundary";
    char *boundary_pos = memmem(data_start, data_len, boundary_hint, strlen(boundary_hint));
    if (boundary_pos) {
        data_len = boundary_pos - data_start;
        ESP_LOGI(TAG, "Trimmed data_len due to boundary at offset %d", (int)(boundary_pos - buffer));
    }

    // Удаляем завершающее CRLF
    if (data_len >= 2 &&
        data_start[data_len - 2] == '\r' &&
        data_start[data_len - 1] == '\n') {
        data_len -= 2;
    }

    // Проверка пределов
    if (total_received + data_len > total_size) {
        data_len = total_size - total_received;
        if (data_len < 0) data_len = 0;
    }

    ESP_LOGI(TAG, "Binary payload length (finalized): %d", data_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data_start + data_len - 32, 32, ESP_LOG_DEBUG);





    // Инициализация OTA при первом чанке
    if (!ota_started) {
        const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!ota_partition) {
            ESP_LOGE(TAG, "No OTA partition found");
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
            return ESP_FAIL;
        }

        esp_err_t ret = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            return ESP_FAIL;
        }

        ota_started = true;
        ESP_LOGI(TAG, "OTA begin on partition: %s", ota_partition->label);
    }

    // запись
    esp_err_t ret = esp_ota_write(ota_handle, data_start, data_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
        free(buffer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }

    total_received += data_len;
    ESP_LOGI(TAG, "Chunk %d written (%d bytes). Total written: %d / %d",
             chunk_number, data_len, total_received, total_size);

    if (total_received >= total_size && total_size > 0) {
        ESP_LOGI(TAG, "=== FINAL CHUNK === total_received=%d / total_size=%d", total_received, total_size);
        ret = esp_ota_end(ota_handle);
        if (ret == ESP_OK) {
            const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
            esp_ota_set_boot_partition(next);
            ESP_LOGI(TAG, "OTA complete. Total written: %d bytes", total_received);
            httpd_resp_send(req, "OTA complete", HTTPD_RESP_USE_STRLEN);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            ESP_LOGI(TAG, "Rebooting...");
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        }
    } else {
        httpd_resp_send(req, "Chunk OK", HTTPD_RESP_USE_STRLEN);
    }

    free(buffer);
    return ESP_OK;
}
