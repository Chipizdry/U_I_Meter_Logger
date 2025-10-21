


#include "nvs_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "esp_mac.h"

#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif


static const char *TAG = "NVS_SETTINGS";
static const char *NAMESPACE = "app_config";



void generate_device_serial(char *serial, size_t size)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(serial, size, "COR-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


esp_err_t nvs_settings_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}


// =======================================================
// ========== Настройки по умолчанию ======================
// =======================================================

static void set_default_user_settings(user_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->version = USER_SETTINGS_VERSION;
    strcpy(s->login, "admin");
    strcpy(s->password, "admin");
    strcpy(s->language, "RU");
    generate_device_serial(s->serial, sizeof(s->serial));
}

static void set_default_network_settings(network_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->version = NETWORK_SETTINGS_VERSION;
    strcpy(s->ssid, "COR-Admin");
    strcpy(s->mode, "AP");
    strcpy(s->ip, "192.168.1.100");
    strcpy(s->gateway, "192.168.1.1");
    strcpy(s->mask, "255.255.255.0");
    strcpy(s->dns, "8.8.8.8");
    s->port =80;
    s->dhcp_enabled = true;
}

static void set_default_system_settings(system_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->version = SYSTEM_SETTINGS_VERSION;
    s->refresh_interval = 1000;
    s->log_level = 2;
    s->debug_mode = false;
    s->build_number = BUILD_NUMBER;
    strncpy(s->build_date, BUILD_DATE, sizeof(s->build_date)-1);
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
        ESP_LOGW(TAG, "No user settings found, loading defaults");
        set_default_user_settings(settings);
        nvs_save_user_settings(settings);
        return ESP_OK;
    }
    if (settings->version != USER_SETTINGS_VERSION) {
        ESP_LOGW(TAG, "User settings version mismatch, resetting to defaults");
        set_default_user_settings(settings);
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
        ESP_LOGW(TAG, "No network settings found, loading defaults");
        set_default_network_settings(settings);
        nvs_save_network_settings(settings);
        return ESP_OK;
    }
    if (settings->version != NETWORK_SETTINGS_VERSION) {
        ESP_LOGW(TAG, "Network settings version mismatch, resetting to defaults");
        set_default_network_settings(settings);
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
        ESP_LOGW(TAG, "No system settings found, loading defaults");
        set_default_system_settings(settings);
        nvs_save_system_settings(settings);
        return ESP_OK;
    }
    if (settings->version != SYSTEM_SETTINGS_VERSION) {
        ESP_LOGW(TAG, "System settings version mismatch, resetting to defaults");
        set_default_system_settings(settings);
        nvs_save_system_settings(settings);
        return ESP_OK;
    }
    return err;
}

esp_err_t nvs_clear_system_settings(void) {
    return nvs_clear_key("system");
}
