#include "ota_update.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mbedtls/md5.h"
#include "web_server.h"
#include "web_auth.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#define BUF_SIZE 4096
#define OTA_CHUNK_SIZE 4096
static const char *TAG = "OTA";

static bool ota_started = false;
static esp_ota_handle_t ota_handle = 0;
static int total_received = 0;
static int total_size = 0;
static const esp_partition_t *ota_partition = NULL;
static void generate_fs_session(fs_session_t *s);

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

    char *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        httpd_resp_send_err(req, 500, "No memory");
        return ESP_FAIL;
    }

    int total_read = 0;
    while (total_read < req->content_len) {
        int r = httpd_req_recv(req, buffer + total_read, req->content_len - total_read);
        if (r <= 0) {
            free(buffer);
            httpd_resp_send_err(req, 500, "Read error");
            return ESP_FAIL;
        }
        total_read += r;
    }
    int received = total_read;



  


    // ============ Извлекаем boundary из заголовка ===============
    char ct[256] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));

    char *boundary_value = NULL;
    char *boundary_marker = NULL;
    char *bpos_hdr = strstr(ct, "boundary=");
    if (bpos_hdr) {
        bpos_hdr += 9;
        if (*bpos_hdr == '"') {
            bpos_hdr++;
            char *endq = strchr(bpos_hdr, '"');
            if (endq) *endq = 0;
        }
        boundary_value = strdup(bpos_hdr);
    }
    if (boundary_value) {
        boundary_marker = malloc(strlen(boundary_value) + 3);
        sprintf(boundary_marker, "--%s", boundary_value);
        ESP_LOGI(TAG, "Boundary: %s", boundary_marker);
    }

    // ============ Ищем поля totalSize, chunkNumber, file =========
    char *total_ptr = memmem(buffer, received, "name=\"totalSize\"", 15);
    char *chunk_ptr = memmem(buffer, received, "name=\"chunkNumber\"", 16);
    char *file_ptr  = memmem(buffer, received, "application/octet-stream", 24);

    if (!total_ptr || !chunk_ptr || !file_ptr) {
        httpd_resp_send_err(req, 400, "Bad multipart");
        free(buffer); free(boundary_value); free(boundary_marker);
        return ESP_FAIL;
    }

    // totalSize
    char *p = total_ptr + 15;
    while (p < buffer+received && !isdigit((unsigned char)*p)) p++;
    int new_total_size = atoi(p);

    // Если OTA уже идёт, но totalSize внезапно другой → сброс
    if (ota_started && new_total_size != total_size) {
        ESP_LOGW(TAG, "Total size changed, resetting OTA");
        ota_cleanup();
    }
    total_size = new_total_size;

    // chunkNumber
    p = chunk_ptr + 16;
    while (p < buffer+received && !isdigit((unsigned char)*p)) p++;
    int chunk_number = atoi(p);

    
      // ===== Проверка токена только для первого чанка =====
      if (chunk_number == 1) {
        if (!check_token(req)) {
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
            free(buffer);
            return ESP_FAIL;
        }
    }

    // 🔥 Не даём перезапустить OTA если она уже идёт
    if (ota_started && total_received > 0 && chunk_number == 1) {
        ESP_LOGW(TAG, "Duplicate chunk 1 ignored (OTA already active)");
        httpd_resp_send(req, "Chunk ignored", -1);
        free(buffer); free(boundary_value); free(boundary_marker);
        return ESP_OK;
    }

    // ===== Находим начало бинарных данных =====
    char *data_start = strstr(file_ptr, "\r\n\r\n");
    if (!data_start) {
        httpd_resp_send_err(req, 400, "No binary marker");
        free(buffer); free(boundary_value); free(boundary_marker);
        return ESP_FAIL;
    }
    data_start += 4;
    int data_len = received - (data_start - buffer);

    // ===== Находим boundary в конце =====
    if (boundary_marker && data_len > 0) {
        size_t boundary_len = strlen(boundary_marker);
    
        for (int i = 0; i <= data_len - (int)boundary_len; i++) {
            if (memcmp(data_start + i, boundary_marker, boundary_len) == 0) {
                bool valid = false;
    
                if (i == 0) valid = true;
                else if (i >= 2 && data_start[i-2] == '\r' && data_start[i-1] == '\n')
                    valid = true;
    
                if (valid) {
                    data_len = i; // отрезаем boundary
    
                    // 🔥 УДАЛЯЕМ CRLF ПЕРЕД boundary если они есть
                    if (data_len >= 2 && data_start[data_len-2] == '\r' && data_start[data_len-1] == '\n') {
                        data_len -= 2;
                    }
    
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "MD5 calc from %d bytes", data_len);
    for (int i = 0; i < 8; i++) {
        printf("%02X ", (uint8_t)data_start[i]);
    }
    printf("...\n");

    ESP_LOGI(TAG, "CHUNK %d -> clean binary: %d bytes", chunk_number, data_len);

    // ========= OTA старт ========== 
    if (!ota_started) {
        ota_partition = esp_ota_get_next_update_partition(NULL);

        ESP_LOGW(TAG, "========== OTA UPDATE ==========");
        ESP_LOGW(TAG, "Writing to partition : %s", ota_partition->label);
        ESP_LOGW(TAG, "Address             : 0x%lx", ota_partition->address);
        ESP_LOGW(TAG, "Size                : 0x%lx", ota_partition->size);
        ESP_LOGW(TAG, "===============================");


        if (!ota_partition) {
            httpd_resp_send_err(req, 500, "No OTA partition");
            free(buffer); free(boundary_value); free(boundary_marker);
            return ESP_FAIL;
        }
        if (esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
            httpd_resp_send_err(req, 500, "OTA begin failed");
            free(buffer); free(boundary_value); free(boundary_marker);
            return ESP_FAIL;
        }
        ota_started = true;
        total_received = 0;
        ESP_LOGI(TAG, "🚀 OTA started");
    }

    // ========= MD5 =========
    char *md5_ptr = memmem(buffer, received, "name=\"md5\"", 10);
    char md5_client[33] = {0};
    if (md5_ptr) {
        char *q = md5_ptr + 10;
        while (q < buffer+received && !isxdigit((unsigned char)*q)) q++;
        for (int i=0; i<32 && q+i<buffer+received && isxdigit((unsigned char)q[i]); i++)
            md5_client[i] = q[i];
    }

    unsigned char md5_bin[16] = {0};
    if (data_len > 0)
        mbedtls_md5((uint8_t*)data_start, data_len, md5_bin);

    char md5_calc[33];
    for (int i=0; i<16; i++) sprintf(md5_calc+i*2, "%02x", md5_bin[i]);

    if (md5_ptr && strcasecmp(md5_client, md5_calc) != 0) {
        ESP_LOGE(TAG, "MD5 MISMATCH %s != %s", md5_client, md5_calc);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "MD5 MISMATCH", -1);
        free(buffer); free(boundary_value); free(boundary_marker);
        return ESP_FAIL;
    }

    /*
    // ========= Запись ==========
    if (data_len > 0) {
        if (esp_ota_write(ota_handle, data_start, data_len) != ESP_OK) {
            httpd_resp_send_err(req, 500, "OTA write failed");
            ota_cleanup();
            free(buffer); free(boundary_value); free(boundary_marker);
            return ESP_FAIL;
        }
        total_received += data_len;
    }
*/

        // ========= Запись ==========
    if (data_len > 0) {
        char *fs_marker_pos = memmem(data_start, data_len, FS_MARKER, sizeof(FS_MARKER)-1);
        if (fs_marker_pos) {
            // Данные до маркера → прошивка
            int fw_len = fs_marker_pos - data_start;
            if (fw_len > 0) {
                if (esp_ota_write(ota_handle, data_start, fw_len) != ESP_OK) {
                    httpd_resp_send_err(req, 500, "OTA write failed");
                    ota_cleanup();
                    free(buffer); free(boundary_value); free(boundary_marker);
                    return ESP_FAIL;
                }
                total_received += fw_len;
            }

            // Данные после маркера → LittleFS
            int fs_len = data_len - fw_len - sizeof(FS_MARKER) + 1; // минус маркер
            if (fs_len > 0) {
                const esp_partition_t *fs_part =
                    esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, NULL);
                if (!fs_part) {
                    ota_cleanup();
                    free(buffer); free(boundary_value); free(boundary_marker);
                    httpd_resp_send_err(req, 500, "No LittleFS partition");
                    return ESP_FAIL;
                }

                ESP_ERROR_CHECK(esp_partition_erase_range(fs_part, 0, fs_part->size));
                ESP_ERROR_CHECK(esp_partition_write(fs_part, 0, fs_marker_pos + sizeof(FS_MARKER), fs_len));
                ESP_LOGI(TAG, "FS written: %d bytes", fs_len);
            }
        } else {
            // Нет маркера → обычная OTA запись
            if (esp_ota_write(ota_handle, data_start, data_len) != ESP_OK) {
                httpd_resp_send_err(req, 500, "OTA write failed");
                ota_cleanup();
                free(buffer); free(boundary_value); free(boundary_marker);
                return ESP_FAIL;
            }
            total_received += data_len;
        }
    }

    // ========= Завершение OTA ==========
    if (total_received >= total_size && ota_started) {
        ESP_LOGI(TAG, "✅ OTA finalize...");
        if (!check_token(req)) {
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
            free(buffer);
            return ESP_FAIL;
        }
        esp_err_t err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed! err=0x%x", err);
            ota_cleanup();
            httpd_resp_send_err(req, 500, "OTA end failed");
            free(buffer); free(boundary_value); free(boundary_marker);
            return ESP_FAIL;
        }

                
        err = esp_ota_set_boot_partition(ota_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
            ota_cleanup();
            httpd_resp_send_err(req, 500, "Set boot partition failed");
            free(buffer); free(boundary_value); free(boundary_marker);
            return ESP_FAIL;
        }

        ESP_LOGW(TAG, "🎉 OTA SUCCESS! Rebooting to new firmware...");
      //  httpd_resp_send(req, "OTA complete, rebooting", -1);

        // Для завершения OTA
        httpd_resp_set_type(req, "application/json");

        char json_resp[128];
        fs_session_t s;
        generate_fs_session(&s);
        sprintf(json_resp, "{\"status\":\"success\",\"ota_session\":\"%s\"}", s.token);
        httpd_resp_sendstr(req, json_resp);
      //  httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"OTA complete, rebooting\"}");

       
        const esp_partition_t *after = esp_ota_get_boot_partition();
        ESP_LOGW(TAG, "🎯 Boot partition set to: %s", after->label);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
                

        free(buffer); free(boundary_value); free(boundary_marker);
        return ESP_OK;
    }

    httpd_resp_send(req, "Chunk OK", -1);

    free(buffer);
    free(boundary_value);
    free(boundary_marker);
    return ESP_OK;
}




 esp_err_t fs_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "=== FS UPDATE ===");

    // Проверка токена FS-сессии
    if (!check_ota_session(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid FS token");
        return ESP_FAIL;
    }

    // Находим раздел LittleFS
    const esp_partition_t *fs_part =
        esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
            NULL);

    if (!fs_part) {
        httpd_resp_send_err(req, 500, "No LittleFS partition");
        return ESP_FAIL;
    }

    // Стираем весь раздел
    ESP_ERROR_CHECK(esp_partition_erase_range(fs_part, 0, fs_part->size));

    uint8_t buf[OTA_CHUNK_SIZE];
    size_t received = 0;

    while (received < req->content_len) {
        int to_read = (req->content_len - received > BUF_SIZE) ? BUF_SIZE : (req->content_len - received);
        int r = httpd_req_recv(req, (char*)buf, to_read);
        if (r <= 0) {
            ESP_LOGE(TAG, "Failed to receive FS data");
            return ESP_FAIL;
        }

        ESP_ERROR_CHECK(esp_partition_write(fs_part, received, buf, r));
        received += r;
    }

    // Одноразово очищаем FS-сессию
    nvs_clear_fs_session();

    httpd_resp_sendstr(req, "FS update OK");
    ESP_LOGW(TAG, "FS UPDATE DONE");

    return ESP_OK;
}



static void generate_fs_session(fs_session_t *s)
{
    memset(s, 0, sizeof(*s));
    generate_token(s->token, sizeof(s->token));
    s->expires = esp_timer_get_time() / 1000000 + 120;
    nvs_save_fs_session(s);
    ESP_LOGW(TAG, "FS SESSION GENERATED: %s", s->token);
}

