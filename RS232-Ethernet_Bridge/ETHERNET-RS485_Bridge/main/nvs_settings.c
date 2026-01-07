


#include "nvs_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "esp_mac.h"


#include "driver/uart.h"

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

      uint8_t mac[6];
    esp_efuse_mac_get_default(mac); 
    memset(s, 0, sizeof(*s));
    s->version = USER_SETTINGS_VERSION;
    strcpy(s->login, "admin");
    strcpy(s->password, "admin");
    snprintf(s->node_name, sizeof(s->node_name), "COR-Bridge-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strcpy(s->account_login, "");
    strcpy(s->account_password, "");
    strcpy(s->language, "RU");
    generate_device_serial(s->serial, sizeof(s->serial));
}



static void set_default_network_settings(network_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->version = NETWORK_SETTINGS_VERSION;
    strcpy(s->ap_ssid, "COR-Bridge");
    strcpy(s->ap_password, "12345678");
    strcpy(s->sta_ssid, " ");
    strcpy(s->sta_password, " ");
    s->mode = WIFI_MODE_AP;
    strcpy(s->ip, "192.168.1.100");
    strcpy(s->gateway, "192.168.1.1");
    strcpy(s->mask, "255.255.255.0");
    strcpy(s->dns, "8.8.8.8");
    s->port =80;
    s->dhcp_enabled = true;
}

static void set_default_wifi_settings(wifi_settings_t *s) {
    memset(s, 0, sizeof(*s));
    strcpy(s->ap_ssid, "COR-Bridge");
    strcpy(s->ap_password, "12345678");
    strcpy(s->sta_ssid, " ");
    strcpy(s->sta_password, " ");
    s->mode = WIFI_MODE_AP;
    strcpy(s->ip, " 192.168.1.1 ");
    strcpy(s->gateway, " 192.168.1.1 ");
    strcpy(s->mask, "255.255.255.0");
    strcpy(s->dns, "8.8.8.8.");
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


static void set_default_uart_settings(uart_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->baud_rate    = 9600;
    s->data_bits    = UART_DATA_8_BITS;      // вместо 8
    s->stop_bits    = UART_STOP_BITS_1;      // вместо 1
    s->parity       = UART_PARITY_DISABLE;   // вместо 'N'
    s->flow_control = false;
    s->rs485_mode   = true;                  // RS485 по умолчанию  
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



// ========= UART настройки =========

esp_err_t nvs_save_uart_settings(const uart_settings_t *settings) {
    return nvs_save_blob("uart", settings, sizeof(uart_settings_t));
}

esp_err_t nvs_load_uart_settings(uart_settings_t *settings) {
    esp_err_t err = nvs_load_blob("uart", settings, sizeof(uart_settings_t));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No UART settings found, loading defaults");
        set_default_uart_settings(settings);
        nvs_save_uart_settings(settings);
        return ESP_OK;
    }

    // Простая защита от мусора в NVS (если baud_rate нереальный — считаем структуру битой)
    if (settings->baud_rate < 300 || settings->baud_rate > 2000000) {
        ESP_LOGW(TAG, "UART settings corrupted, resetting to defaults");
        set_default_uart_settings(settings);
        nvs_save_uart_settings(settings);
        return ESP_OK;
    }

    return err;
}

esp_err_t nvs_clear_uart_settings(void) {
    return nvs_clear_key("uart");
}


// ========= WiFi настройки =========

esp_err_t nvs_save_wifi_settings(const wifi_settings_t *settings) {
    return nvs_save_blob("wifi", settings, sizeof(wifi_settings_t));
}
esp_err_t nvs_load_wifi_settings(wifi_settings_t *settings) {
    esp_err_t err = nvs_load_blob("wifi", settings, sizeof(wifi_settings_t));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No WiFi settings found, loading defaults");
        set_default_wifi_settings(settings);
        nvs_save_wifi_settings(settings);
        return ESP_OK;
    }
    return err;
}
esp_err_t nvs_clear_wifi_settings(void) {
    return nvs_clear_key("wifi");
}


// ========= FS сессия =========
esp_err_t nvs_save_fs_session(const fs_session_t *s)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("ota", NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, "fs_session", s, sizeof(*s)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    return ESP_OK;
}

esp_err_t nvs_load_fs_session(fs_session_t *s)
{
    size_t len = sizeof(*s);
    nvs_handle_t h;
    esp_err_t err = nvs_open("ota", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, "fs_session", s, &len);
    nvs_close(h);
    return err;
}

void nvs_clear_fs_session(void)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "fs_session");
        nvs_commit(h);
        nvs_close(h);
    }
}


void system_update_build_info(void)
{
    if (sys.build_number == BUILD_NUMBER &&
        strcmp(sys.build_date, BUILD_DATE) == 0) {
        // Прошивка та же — ничего не делаем
        return;
    }

    ESP_LOGW("SYS", "New firmware detected");
    ESP_LOGW("SYS", "Old build: %d (%s)", sys.build_number, sys.build_date);
    ESP_LOGW("SYS", "New build: %d (%s)", BUILD_NUMBER, BUILD_DATE);

    sys.build_number = BUILD_NUMBER;
    strncpy(sys.build_date, BUILD_DATE, sizeof(sys.build_date) - 1);
    sys.build_date[sizeof(sys.build_date) - 1] = 0;

    esp_err_t err = nvs_save_system_settings(&sys);
    if (err != ESP_OK) {
        ESP_LOGE("SYS", "Failed to save system settings: %s",
                 esp_err_to_name(err));
    }
}