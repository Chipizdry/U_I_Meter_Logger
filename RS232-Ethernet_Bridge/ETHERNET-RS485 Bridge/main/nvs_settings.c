


#include "nvs_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_SETTINGS";
static const char *NAMESPACE = "app_config";

esp_err_t nvs_settings_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

// ========= универсальная функция =========
static esp_err_t nvs_save_blob(const char *key, const void *data, size_t size) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_load_blob(const char *key, void *data, size_t size) {
    nvs_handle_t handle;
    size_t len = size;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_blob(handle, key, data, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_clear_key(const char *key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_erase_key(handle, key);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// ========= Пользовательские настройки =========

esp_err_t nvs_save_user_settings(const user_settings_t *settings) {
    return nvs_save_blob("user", settings, sizeof(user_settings_t));
}

esp_err_t nvs_load_user_settings(user_settings_t *settings) {
    esp_err_t err = nvs_load_blob("user", settings, sizeof(user_settings_t));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No user settings found, creating default");
        memset(settings, 0, sizeof(*settings));
        settings->version = USER_SETTINGS_VERSION;
        strcpy(settings->login, "user@example.com");
        strcpy(settings->password, "12345678");
        strcpy(settings->language, "en");
        nvs_save_user_settings(settings);
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_clear_user_settings(void) {
    return nvs_clear_key("user");
}

// ========= Сетевые настройки =========

esp_err_t nvs_save_network_settings(const network_settings_t *settings) {
    return nvs_save_blob("network", settings, sizeof(network_settings_t));
}

esp_err_t nvs_load_network_settings(network_settings_t *settings) {
    esp_err_t err = nvs_load_blob("network", settings, sizeof(network_settings_t));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No network settings found, creating default");
        memset(settings, 0, sizeof(*settings));
        settings->version = NETWORK_SETTINGS_VERSION;
        strcpy(settings->ip, "192.168.1.100");
        strcpy(settings->gateway, "192.168.1.1");
        strcpy(settings->dns, "8.8.8.8");
        settings->dhcp_enabled = true;
        nvs_save_network_settings(settings);
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_clear_network_settings(void) {
    return nvs_clear_key("network");
}

// ========= Системные настройки =========

esp_err_t nvs_save_system_settings(const system_settings_t *settings) {
    return nvs_save_blob("system", settings, sizeof(system_settings_t));
}

esp_err_t nvs_load_system_settings(system_settings_t *settings) {
    esp_err_t err = nvs_load_blob("system", settings, sizeof(system_settings_t));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No system settings found, creating default");
        memset(settings, 0, sizeof(*settings));
        settings->version = SYSTEM_SETTINGS_VERSION;
        settings->refresh_interval = 1000;
        settings->log_level = 2;
        settings->debug_mode = false;
        nvs_save_system_settings(settings);
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_clear_system_settings(void) {
    return nvs_clear_key("system");
}


