


#include "nvs_credentials.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_CRED";
static const char *NAMESPACE = "credentials";

esp_err_t nvs_credentials_init(void) {
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_LOGI(TAG, "NVS init returned 0x%x", ret);
    return ret;
}

esp_err_t nvs_save_credentials(const char *login, const char *password) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return err;
    }

    err = nvs_set_str(handle, "login", login);
    if (err != ESP_OK) goto cleanup;
    err = nvs_set_str(handle, "password", password);
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "Credentials saved");

cleanup:
    nvs_close(handle);
    return err;
}

esp_err_t nvs_load_credentials(char *login, size_t login_size, char *password, size_t password_size) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return err;
    }

    size_t len = login_size;
    err = nvs_get_str(handle, "login", login, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = password_size;
    err = nvs_get_str(handle, "password", password, &len);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_clear_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_erase_key(handle, "login");
    nvs_erase_key(handle, "password");
    err = nvs_commit(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "Credentials cleared");
    nvs_close(handle);
    return err;
}



