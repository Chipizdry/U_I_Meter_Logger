#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "littlefs_manager.h"
#include <dirent.h>

static const char *TAG = "littlefs";

// ======================= INIT =======================
esp_err_t littlefs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL)
            ESP_LOGE(TAG, "Failed to mount or format LittleFS");
        else if (ret == ESP_ERR_NOT_FOUND)
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        else
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition info (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LittleFS mounted successfully");
    ESP_LOGI(TAG, "Partition size: total: %d bytes, used: %d bytes", total, used);
    return ESP_OK;
}

// ======================= FILE LIST =======================
void littlefs_list_files(void)
{
    DIR *dir = opendir("/littlefs");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory");
        return;
    }

    ESP_LOGI(TAG, "Files in /littlefs:");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, " - %s", entry->d_name);
    }
    closedir(dir);
}

// ======================= WRITE FILE =======================
esp_err_t littlefs_write_file(const char *path, const char *data)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/littlefs/%s", path);

    FILE *f = fopen(full_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path);
        return ESP_FAIL;
    }

    fprintf(f, "%s", data);
    fclose(f);
    ESP_LOGI(TAG, "File written: %s", full_path);
    return ESP_OK;
}

// ======================= READ FILE =======================
esp_err_t littlefs_read_file(const char *path)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/littlefs/%s", path);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Contents of %s:", full_path);
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }
    fclose(f);
    return ESP_OK;
}

// ======================= DELETE FILE =======================
esp_err_t littlefs_delete_file(const char *path)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/littlefs/%s", path);

    if (remove(full_path) == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", full_path);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        return ESP_FAIL;
    }
}
