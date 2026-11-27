

#include "ws_settings_manager.h"

#include "esp_log.h"

static const char *TAG = "WS_settings_manager";

esp_err_t ws_settings_save_account(const user_settings_t *cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "Cannot save account settings: cfg is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,"Saving account settings: node_name='%s', login='%s'", cfg->node_name, cfg->account_login);
    esp_err_t err = nvs_save_user_settings(cfg);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to save account settings to NVS: %s",
                 esp_err_to_name(err));

        return err;
    }

    return ESP_OK;
}


esp_err_t ws_settings_save_uart(const uart_settings_t *cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "Cannot save UART settings: cfg is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,
             "Saving UART settings: baud=%d, data=%d, stop=%d, parity=%d, mode=%s",
             cfg->baud_rate,
             cfg->data_bits,
             cfg->stop_bits,
             cfg->parity,
             cfg->rs485_mode ? "RS485" : "RS232");

    esp_err_t err = nvs_save_uart_settings(cfg);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Failed to save UART settings to NVS: %s",
                 esp_err_to_name(err));

        return err;
    }

    ESP_LOGI(TAG, "UART settings saved to NVS");

    return ESP_OK;
}

esp_err_t ws_settings_apply_account(cJSON *account, user_settings_t *cfg)
{
    if (account == NULL || cfg == NULL)
    {
        ESP_LOGE(TAG, "Invalid account arguments");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *account_obj = account;
    bool account_json_allocated = false;

    /*
     * Frontend может отправить:
     *
     * "account": {
     *     "node_name": "...",
     *     "login": "...",
     *     "password": "..."
     * }
     *
     * или:
     *
     * "account": "{\"node_name\":\"...\", ...}"
     */
    if (cJSON_IsString(account))
    {
        account_obj = cJSON_Parse(account->valuestring);

        if (account_obj == NULL)
        {
            ESP_LOGE(TAG, "Failed to parse account JSON string");
            return ESP_ERR_INVALID_ARG;
        }

        account_json_allocated = true;
    }

    if (!cJSON_IsObject(account_obj))
    {
        ESP_LOGE(TAG, "Invalid account settings format");

        if (account_json_allocated)
        {
            cJSON_Delete(account_obj);
        }

        return ESP_ERR_INVALID_ARG;
    }

    cJSON *node_name = cJSON_GetObjectItem(account_obj, "node_name");
    cJSON *login     = cJSON_GetObjectItem(account_obj, "login");
    cJSON *password  = cJSON_GetObjectItem(account_obj, "password");

    if (cJSON_IsString(node_name))
    {
        strlcpy(
            cfg->node_name,
            node_name->valuestring,
            sizeof(cfg->node_name)
        );
    }

    if (cJSON_IsString(login))
    {
        strlcpy(
            cfg->account_login,
            login->valuestring,
            sizeof(cfg->account_login)
        );
    }

    if (cJSON_IsString(password))
    {
        strlcpy(
            cfg->account_password,
            password->valuestring,
            sizeof(cfg->account_password)
        );
    }

    ESP_LOGI(
        TAG,
        "Account applied: node_name='%s', login='%s'",
        cfg->node_name,
        cfg->account_login
    );

    esp_err_t err = ws_settings_save_account(cfg);

    if (account_json_allocated)
    {
        cJSON_Delete(account_obj);
    }

    return err;
}


esp_err_t ws_settings_apply_uart(cJSON *uart_item, uart_settings_t *cfg)
{
    if (uart_item == NULL || cfg == NULL)
    {
        ESP_LOGE(TAG, "Invalid UART arguments");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *uart_json = uart_item;
    bool uart_json_allocated = false;

    /*
     * UART может прийти как объект
     * или как JSON-строка.
     */
    if (cJSON_IsString(uart_item))
    {
        uart_json = cJSON_Parse(uart_item->valuestring);

        if (uart_json == NULL)
        {
            ESP_LOGE(TAG, "Failed to parse UART JSON string");
            return ESP_ERR_INVALID_ARG;
        }

        uart_json_allocated = true;
    }

    if (!cJSON_IsObject(uart_json))
    {
        ESP_LOGE(TAG, "Invalid UART settings format");

        if (uart_json_allocated)
        {
            cJSON_Delete(uart_json);
        }

        return ESP_ERR_INVALID_ARG;
    }

    cJSON *baud      = cJSON_GetObjectItem(uart_json, "baud");
    cJSON *data_bits = cJSON_GetObjectItem(uart_json, "data_bits");
    cJSON *stop_bits = cJSON_GetObjectItem(uart_json, "stop_bits");
    cJSON *parity    = cJSON_GetObjectItem(uart_json, "parity");
    cJSON *mode      = cJSON_GetObjectItem(uart_json, "mode");

    /* ------------------------------------------------------
     * BAUD
     * ------------------------------------------------------ */

    if (cJSON_IsNumber(baud))
    {
        cfg->baud_rate = baud->valueint;
    }

    /* ------------------------------------------------------
     * DATA BITS
     * ------------------------------------------------------ */

    if (cJSON_IsNumber(data_bits))
    {
        switch (data_bits->valueint)
        {
            case 5:
                cfg->data_bits = UART_DATA_5_BITS;
                break;

            case 6:
                cfg->data_bits = UART_DATA_6_BITS;
                break;

            case 7:
                cfg->data_bits = UART_DATA_7_BITS;
                break;

            case 8:
                cfg->data_bits = UART_DATA_8_BITS;
                break;

            default:
                ESP_LOGW(
                    TAG,
                    "Invalid data_bits=%d, using 8",
                    data_bits->valueint
                );

                cfg->data_bits = UART_DATA_8_BITS;
                break;
        }
    }

    /* ------------------------------------------------------
     * STOP BITS
     * ------------------------------------------------------ */

    if (cJSON_IsNumber(stop_bits))
    {
        switch (stop_bits->valueint)
        {
            case 1:
                cfg->stop_bits = UART_STOP_BITS_1;
                break;

            case 2:
                cfg->stop_bits = UART_STOP_BITS_2;
                break;

            case 15:
                cfg->stop_bits = UART_STOP_BITS_1_5;
                break;

            default:
                ESP_LOGW(
                    TAG,
                    "Invalid stop_bits=%d, using 1",
                    stop_bits->valueint
                );

                cfg->stop_bits = UART_STOP_BITS_1;
                break;
        }
    }

    /* ------------------------------------------------------
     * PARITY
     * ------------------------------------------------------ */

    if (cJSON_IsNumber(parity))
    {
        switch (parity->valueint)
        {
            case UART_PARITY_DISABLE:
            case UART_PARITY_EVEN:
            case UART_PARITY_ODD:

                cfg->parity = parity->valueint;
                break;

            default:

                ESP_LOGW(
                    TAG,
                    "Invalid parity=%d, using disabled",
                    parity->valueint
                );

                cfg->parity = UART_PARITY_DISABLE;
                break;
        }
    }

    /* ------------------------------------------------------
     * RS485 / RS232
     * ------------------------------------------------------ */

    if (cJSON_IsNumber(mode))
    {
        cfg->rs485_mode = mode->valueint ? 1 : 0;
    }

    ESP_LOGI(
        TAG,
        "UART applied: baud=%d data=%d stop=%d parity=%d mode=%s",
        cfg->baud_rate,
        cfg->data_bits,
        cfg->stop_bits,
        cfg->parity,
        cfg->rs485_mode ? "RS485" : "RS232"
    );

    esp_err_t err = ws_settings_save_uart(cfg);

    if (uart_json_allocated)
    {
        cJSON_Delete(uart_json);
    }

    return err;
}



esp_err_t ws_settings_apply_user(cJSON *user_item, user_settings_t *cfg)
{
    if (user_item == NULL || cfg == NULL)
    {
        ESP_LOGE(TAG, "Invalid user arguments");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *user_json = user_item;
    bool user_json_allocated = false;

    if (cJSON_IsString(user_item))
    {
        user_json = cJSON_Parse(user_item->valuestring);

        if (user_json == NULL)
        {
            ESP_LOGE(TAG, "Failed to parse user JSON string");
            return ESP_ERR_INVALID_ARG;
        }

        user_json_allocated = true;
    }

    if (!cJSON_IsObject(user_json))
    {
        ESP_LOGE(TAG, "Invalid user settings format");

        if (user_json_allocated)
        {
            cJSON_Delete(user_json);
        }

        return ESP_ERR_INVALID_ARG;
    }

    cJSON *login    = cJSON_GetObjectItem(user_json, "login");
    cJSON *password = cJSON_GetObjectItem(user_json, "password");
    cJSON *language = cJSON_GetObjectItem(user_json, "language");

    if (cJSON_IsString(login))
    {
        strlcpy(cfg->login,login->valuestring,sizeof(cfg->login));
    }

    if (cJSON_IsString(password))
    {
        strlcpy(cfg->password, password->valuestring,sizeof(cfg->password));
    }

    if (cJSON_IsString(language))
    {
        strlcpy(cfg->language,language->valuestring, sizeof(cfg->language));
    }


    ESP_LOGI(TAG, "User settings applied: login='%s', language='%s' ",cfg->login, cfg->language);
    esp_err_t err = nvs_save_user_settings(cfg);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to save user settings: %s", esp_err_to_name(err));
    }

    if (user_json_allocated)
    {
        cJSON_Delete(user_json);
    }

    return err;
}



