#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mbedtls/md5.h"


#define OTA_CHUNK_SIZE 4096
static const char *TAG = "OTA";

static bool ota_started = false;
static esp_ota_handle_t ota_handle = 0;
static int total_received = 0;
static int total_size = 0;
static const esp_partition_t *ota_partition = NULL;

void ota_init(void) {
    ota_started = false;
    total_received = 0;
    ota_handle = 0;
    ota_partition = NULL;
    total_size = 0;
}

/** Сброс OTA состояния при ошибке */
static void ota_cleanup(void) {
    if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
    ota_started = false;
    total_received = 0;
    ota_partition = NULL;
    total_size = 0;
}

esp_err_t ota_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== OTA POST === len=%d total_received=%d ===", req->content_len, total_received);

    // === Логирование на каждом этапе ===
    ESP_LOGI(TAG, "[DEBUG] Received POST request: len=%d, total_received=%d", req->content_len, total_received);

    char *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "❌ Failed to allocate OTA buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory for OTA buffer");
        return ESP_FAIL;
    }

    int total_read = 0;
    while (total_read < req->content_len) {
        int received = httpd_req_recv(req, buffer + total_read, req->content_len - total_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "❌ Error reading OTA chunk (received=%d, total_read=%d)", received, total_read);
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read error");
            return ESP_FAIL;
        }
        total_read += received;
    }

    int received = total_read;
    ESP_LOGI(TAG, "📥 Full POST body read: %d bytes", received);

    // === Проверка целостности данных ===
    if (received < 100) { // Минимальный размер для multipart данных
        ESP_LOGE(TAG, "❌ Received data too small: %d bytes", received);
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Data too small");
        return ESP_FAIL;
    }

    // === Поиск multipart полей ===
    char *total_ptr = memmem(buffer, received, "name=\"totalSize\"", strlen("name=\"totalSize\""));
    char *chunk_ptr = memmem(buffer, received, "name=\"chunkNumber\"", strlen("name=\"chunkNumber\""));
    char *file_ptr  = memmem(buffer, received, "application/octet-stream", strlen("application/octet-stream"));
    ESP_LOGI(TAG, "🔍 Field positions: total_ptr=%p chunk_ptr=%p file_ptr=%p", total_ptr, chunk_ptr, file_ptr);

 
    if (!total_ptr || !chunk_ptr || !file_ptr) {
        ESP_LOGE(TAG, "❌ Multipart fields missing");
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid multipart data");
        return ESP_FAIL;
    }

    // === Извлекаем totalSize ===
    char total_buf[32] = {0};
    char *p = total_ptr + strlen("name=\"totalSize\"");
    while ((p - buffer) < received && *p && !isdigit((int)*p)) p++;
    for (int i = 0; i < (int)sizeof(total_buf) - 1 && (p - buffer) + i < received && isdigit((int)p[i]); i++)
        total_buf[i] = p[i];
    int new_total_size = atoi(total_buf);

    // === Если total_size изменился - это новая OTA сессия ===
    if (ota_started && new_total_size != total_size) {
        ESP_LOGW(TAG, "⚠️ Total size changed (%d -> %d), resetting OTA", total_size, new_total_size);
        ota_cleanup();
    }
    total_size = new_total_size;

    // === Извлекаем chunkNumber ===
    char chunk_buf[32] = {0};
    p = chunk_ptr + strlen("name=\"chunkNumber\"");
    while ((p - buffer) < received && *p && !isdigit((int)*p)) p++;
    for (int i = 0; i < (int)sizeof(chunk_buf) - 1 && (p - buffer) + i < received && isdigit((int)p[i]); i++)
        chunk_buf[i] = p[i];
    int chunk_number = atoi(chunk_buf);

    ESP_LOGI(TAG, "📑 Parsed fields: chunk=%d, totalSize=%d", chunk_number, total_size);

    // --- Находим начало бинарных данных ---
    char *data_start = strstr(file_ptr, "\r\n\r\n");
    if (!data_start) {
        ESP_LOGE(TAG, "❌ No binary data marker found");
        free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No binary data");
        return ESP_FAIL;
    }
    data_start += 4;
    // --- Предполагаем максимальную длину ---
    int data_len = received - (int)(data_start - buffer);

        // --- Ищем СЛЕДУЮЩУЮ boundary строку ---
        const char *boundary = "\r\n------WebKitFormBoundary";
        char *bpos = memmem(data_start, data_len, boundary, strlen(boundary));
        bool is_final_boundary = false;

        if (bpos && bpos > data_start) {
            data_len = (int)(bpos - data_start);
        }
        
        while (data_len > 0 &&
            (data_start[data_len - 1] == '\r' || data_start[data_len - 1] == '\n')) {
          data_len--;
      }
      
      ESP_LOGI(TAG, "✅ Binary cleaned: data_len=%d bytes", data_len);



    // === Убираем хвостовые CR/LF ===
    while (data_len > 0 &&
          (data_start[data_len - 1] == '\r' || data_start[data_len - 1] == '\n')) {
        data_len--;
    }

    bool final_chunk = is_final_boundary;
    ESP_LOGI(TAG, "📏 Final data_len=%d (final_chunk=%d)", data_len, final_chunk);

    // === Инициализация OTA ===
    if (!ota_started) {
        ESP_LOGI(TAG, "🚀 Starting OTA...");
        ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!ota_partition) {
            ESP_LOGE(TAG, "❌ No OTA partition found");
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
            return ESP_FAIL;
        }
        esp_err_t ret = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ esp_ota_begin failed: %s", esp_err_to_name(ret));
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            return ESP_FAIL;
        }
        ota_started = true;
        total_received = 0;
        ESP_LOGI(TAG, "✅ OTA begin OK on partition: %s", ota_partition->label);
    }

    // === Запись чанка ===
    if (data_len > 0) {
        ESP_LOGI(TAG, "Writing chunk: data_len=%d, total_received=%d", data_len, total_received);

        // Логирование перед записью данных
        ESP_LOGI(TAG, "[DEBUG] Writing chunk: chunk_number=%d, data_len=%d, total_received=%d", chunk_number, data_len, total_received);

   

        // Убираем хвостовые символы \r\n
        while (data_len > 0 &&
              (data_start[data_len - 1] == '\r' || data_start[data_len - 1] == '\n')) {
            data_len--;
        }


          // --- 1) извлечь md5 из multipart (поле name="md5") ---
        char *md5_ptr = memmem(buffer, received, "name=\"md5\"", strlen("name=\"md5\""));
        char md5_hex_from_client[33] = {0};
        if (md5_ptr) {
            // Найдём начало цифр (например: name="md5"\r\n\r\n<32 hex chars>)
            char *q = md5_ptr + strlen("name=\"md5\"");
            // сдвинуть до первой hex-цифры
            while ((q - buffer) < received && *q && !isxdigit((unsigned char)*q)) q++;
            // копируем до 32 hex символов или до конца
            int hn = 0;
            while (hn < 32 && (q - buffer) + hn < received && isxdigit((unsigned char)q[hn])) {
                md5_hex_from_client[hn] = q[hn];
                hn++;
            }
            md5_hex_from_client[hn] = '\0';
        }

        // --- 2) вычислить MD5 от бинарного блока (data_start, data_len) ---
        unsigned char md5_calc[16];
        if (data_len > 0) {
            mbedtls_md5_context ctx;
            mbedtls_md5_init(&ctx);
            mbedtls_md5_starts(&ctx);
            mbedtls_md5_update(&ctx, (const unsigned char*)data_start, (size_t)data_len);
            mbedtls_md5_finish(&ctx, md5_calc);
            mbedtls_md5_free(&ctx);
        } else {
            memset(md5_calc, 0, sizeof(md5_calc));
        }

        // --- 3) преобразовать md5_calc в hex-строку и сравнить (если пришёл от клиента) ---
        char md5_hex_calc[33];
        for (int i = 0; i < 16; ++i) {
            sprintf(&md5_hex_calc[i*2], "%02x", md5_calc[i]);
        }
        md5_hex_calc[32] = '\0';

        if (md5_ptr) {
            if (strlen(md5_hex_from_client) != 32) {
                ESP_LOGW(TAG, "Client md5 has invalid length (%d)", (int)strlen(md5_hex_from_client));
                // попросим клиент повторить
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_send(req, "MD5 INVALID", HTTPD_RESP_USE_STRLEN);
                free(buffer);
                return ESP_FAIL;
            }
            // сравнение
            if (strcasecmp(md5_hex_from_client, md5_hex_calc) != 0) {
                ESP_LOGW(TAG, "MD5 MISMATCH chunk=%d client=%s calc=%s", chunk_number, md5_hex_from_client, md5_hex_calc);
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_send(req, "MD5 MISMATCH", HTTPD_RESP_USE_STRLEN);
                free(buffer);
                return ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "MD5 OK chunk=%d md5=%s", chunk_number, md5_hex_calc);
            }
        } else {
            ESP_LOGW(TAG, "Client didn't send MD5 for chunk %d — proceeding without check", chunk_number);
        }


        // Проверка корректности записи данных
        esp_err_t write_ret = esp_ota_write(ota_handle, data_start, data_len);
        if (write_ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ esp_ota_write failed: %s", esp_err_to_name(write_ret));
            ota_cleanup();
            free(buffer);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "[DEBUG] Chunk written successfully: data_len=%d, total_received=%d", data_len, total_received);
        total_received += data_len;
    }

    // Логирование после записи данных
 

    ESP_LOGI(TAG, "📊 After write: total_received=%d / %d", total_received, total_size);

    // === Завершение OTA ===
    if (total_received >= total_size - 1) { // Добавлен допуск в 1 байт
        ESP_LOGI(TAG, "🎯 All chunks received (%d / %d). Finalizing OTA.", total_received, total_size);

        // Логирование при завершении OTA
        ESP_LOGI(TAG, "[DEBUG] Finalizing OTA: total_received=%d, total_size=%d", total_received, total_size);

        esp_err_t end_ret = esp_ota_end(ota_handle);
        if (end_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ OTA validation successful");
            esp_err_t set_ret = esp_ota_set_boot_partition(ota_partition);
            if (set_ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ Boot partition set. Total written: %d bytes", total_received);
                httpd_resp_send(req, "OTA complete", HTTPD_RESP_USE_STRLEN);
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                esp_restart();
            } else {
                ESP_LOGE(TAG, "❌ Failed to set boot partition: %s", esp_err_to_name(set_ret));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
            }
        } else {
            ESP_LOGE(TAG, "❌ esp_ota_end failed: %s", esp_err_to_name(end_ret));
            ota_cleanup(); // Сброс при неудачной валидации
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        }
        free(buffer);
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "📊 Chunk processed: total_received=%d / %d", total_received, total_size);
        httpd_resp_send(req, "Chunk OK", HTTPD_RESP_USE_STRLEN);
    }

    free(buffer);
    return ESP_OK;
}


