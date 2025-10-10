







#include "rs485_master.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h> 
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h" 
#include "esp_timer.h"

static const char *TAG = "rs485_master";

/* --- внутренние структуры --- */
typedef struct {
    rs485_slave_cfg_t cfg;
    rs485_slave_data_t data;
    TickType_t last_poll_tick;
    bool in_use;
} slave_entry_t;

static slave_entry_t s_slaves[RS485_MAX_SLAVES];
static int s_slave_count = 0;

static int s_baud = 9600;
static int s_rx_buf = 1024;
static int s_tx_buf = 256;

static TaskHandle_t s_poll_task = NULL;
static SemaphoreHandle_t s_data_mutex = NULL;
static SemaphoreHandle_t s_uart_mutex = NULL;
static volatile bool s_running = false;

/* CRC16 (Modbus) */
static uint16_t modbus_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

static void de_off_timer_cb(void *arg)
{
    // просто опускаем DE
    rs485_set_de(0);
    ESP_LOGD(TAG, "DE → 0 (timer expired)");
}





/* Управление DE (direction) */
static inline void rs485_set_de(int level)
{
    gpio_set_level(RS485_DE_PIN, level ? 1 : 0);
}

/* Вычислить время одного символа в микросекундах:
   берем 1 старт + 8 data + 1 stop = 10 бит per char (для 8N1).
   char_time_us = 10 * 1e6 / baud
*/
static inline uint32_t char_time_us_from_baud(int baud)
{
    // вычисляем точно: 10 bits per char
    // 10/baud seconds -> *1e6 -> microseconds
    // делаем целочисленно:
    // char_time_us = (10 * 1000000) / baud
    uint32_t numerator = 10U * 1000000U; // 10,000,000
    return (uint32_t)(numerator / (uint32_t)baud);
}

/* Построение Modbus RTU read holding registers (Функция 0x03)
   request: addr | func | start_hi | start_lo | count_hi | count_lo | crc_lo | crc_hi
*/
static int build_read_request(uint8_t addr, uint16_t start, uint16_t count, uint8_t *out, size_t out_len)
{
    if (out_len < 8) return -1;
    out[0] = addr;
    out[1] = 0x03;
    out[2] = (uint8_t)((start >> 8) & 0xFF);
    out[3] = (uint8_t)(start & 0xFF);
    out[4] = (uint8_t)((count >> 8) & 0xFF);
    out[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = modbus_crc16(out, 6);
    out[6] = crc & 0xFF;
    out[7] = (crc >> 8) & 0xFF;
    return 8;
}

/* Чтение ответа синхронно: ожидаем конкретное количество байт.
   expected_len = 1(addr)+1(func)+1(byte_count)+2*count(data)+2(crc)
*/
static int read_response_sync(int uart_num, uint8_t *buf, int expected_len, int timeout_ms)
{
    if (expected_len <= 0) return -1;
    int to_read = expected_len;
    int r = uart_read_bytes(uart_num, buf, to_read, pdMS_TO_TICKS(timeout_ms));
    if (r < 0) return -1;
    return r;
}

/* Отправка запроса: берём uart_mutex чтобы избежать конкуренции, ставим DE=1, пишем, ждем TX done, даём DE=0 */
static esp_err_t send_request_async(int uart_num, const uint8_t *req, int req_len, uint32_t char_time_us)
{
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "UART busy");
        return ESP_ERR_TIMEOUT;
    }

    // Время передачи сообщения в микросекундах:
    // (длина пакета в байтах) * (10 бит/байт) * (1e6 / baud)
    // char_time_us уже = 10 * 1e6 / baud
    uint64_t tx_time_us = (uint64_t)req_len * (uint64_t)char_time_us;

    // Добавим запас в 3 символа:
    uint64_t total_time_us = tx_time_us + (uint64_t)(3 * char_time_us);

    // Поднимаем DE
    rs485_set_de(1);

    // Передача пакета (не ждём завершения)
    int written = uart_write_bytes(uart_num, (const char *)req, req_len);
    if (written != req_len) {
        ESP_LOGW(TAG, "uart_write_bytes wrote %d/%d", written, req_len);
    }

    // Запускаем одноразовый таймер, который опустит DE после total_time_us
    const esp_timer_create_args_t targs = {
        .callback = &de_off_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "de_off_timer"
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, total_time_us));

    ESP_LOGI(TAG, "TX started (%d bytes), DE→1 for ~%" PRIu64 " us", req_len, total_time_us);
    ESP_LOG_BUFFER_HEX(TAG, req, req_len);

    xSemaphoreGive(s_uart_mutex);
    return ESP_OK;
}

/* Single poll operation on a slave: отправка запроса и чтение ответа, update data */
static void poll_slave(slave_entry_t *s)
{
    const int uart_num = RS485_UART_NUM;
    uint8_t req[8];
    int nreq = build_read_request(s->cfg.slave_addr, s->cfg.reg_start, s->cfg.reg_count, req, sizeof(req));
    if (nreq < 0) {
        s->data.last_error = -2;
        return;
    }

    uint32_t char_time_us = char_time_us_from_baud(s_baud);
    // Отправляем запрос синхронно (DE управление внутри)
    esp_err_t er = send_request_async(uart_num, req, nreq, char_time_us);
    if (er != ESP_OK) {
        s->data.last_error = -3;
        return;
    }

    // Ожидаем ответ: expected_len = 5 + 2*count
    int expected_len = 5 + 2 * s->cfg.reg_count;
    // Вычисляем таймаут: минимум 3.5 char для silent + time to receive expected bytes
    // time to receive expected_len chars = expected_len * char_time_us
    uint32_t t_receive_us = (uint32_t)expected_len * char_time_us;
    // silent interval = 3.5 * char_time_us (rounded up)
    uint32_t t_silent_us = (uint32_t)(3.5 * (double)char_time_us + 0.5);
    // total timeout in microseconds:
    uint32_t total_us = t_silent_us + t_receive_us + 1000000U; // добавим запас 1s (на случай потерь)
    // Перевод в миллисекунды, минимум 20ms
    int timeout_ms = (int)(total_us / 1000U);
    if (timeout_ms < 20) timeout_ms = 20;
    if (timeout_ms > 5000) timeout_ms = 5000;

    uint8_t *resp = (uint8_t *)heap_caps_malloc(expected_len + 4, MALLOC_CAP_8BIT);
    if (!resp) {
        s->data.last_error = -4;
        return;
    }
    memset(resp, 0, expected_len + 4);

    int r = read_response_sync(uart_num, resp, expected_len, timeout_ms);
    if (r <= 0) {
        s->data.last_error = -5; // timeout/no data
        heap_caps_free(resp);
        return;
    }
    ESP_LOGI(TAG, "RX (%d bytes) ← slave %d:", r, s->cfg.slave_addr);
    ESP_LOG_BUFFER_HEX(TAG, resp, r);
    // Проверка адреса и функция
    if (r < 5) {
        s->data.last_error = -6;
        heap_caps_free(resp);
        return;
    }
    if (resp[0] != s->cfg.slave_addr || resp[1] != 0x03) {
        s->data.last_error = -7;
        heap_caps_free(resp);
        return;
    }
    int byte_count = resp[2];
    if (byte_count != 2 * s->cfg.reg_count) {
        s->data.last_error = -8;
        heap_caps_free(resp);
        return;
    }
    // CRC check: CRC is last two bytes in response (low, high)
    uint16_t crc_calc = modbus_crc16(resp, 3 + byte_count);
    uint16_t crc_recv = (uint16_t)resp[3 + byte_count] | ((uint16_t)resp[3 + byte_count + 1] << 8);
    if (crc_calc != crc_recv) {
        s->data.last_error = -9;
        heap_caps_free(resp);
        return;
    }

    // Парсинг данных (big-endian words)
    if (s->cfg.reg_count > (int)(sizeof(s->data.values) / sizeof(s->data.values[0]))) {
        s->data.last_error = -10;
        heap_caps_free(resp);
        return;
    }

    // Копируем в структуру под мьютексом
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s->data.slave_addr = s->cfg.slave_addr;
        s->data.reg_start = s->cfg.reg_start;
        s->data.reg_count = s->cfg.reg_count;
        uint8_t *p = &resp[3];
        for (int i = 0; i < s->cfg.reg_count; ++i) {
            uint16_t hi = p[2*i];
            uint16_t lo = p[2*i + 1];
            s->data.values[i] = (uint16_t)((hi << 8) | lo);
        }
        s->data.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s->data.last_error = 0;
        xSemaphoreGive(s_data_mutex);
    } else {
        s->data.last_error = -11;
    }

    heap_caps_free(resp);
}

/* Polling task: идёт по всем слейвам и опрашивает те, у которых время пришло */
static void poll_task(void *arg)
{
   // const int uart_num = RS485_UART_NUM;
    while (s_running) {
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < RS485_MAX_SLAVES; ++i) {
            if (!s_slaves[i].in_use) continue;
            TickType_t interval_ticks = pdMS_TO_TICKS(s_slaves[i].cfg.poll_interval_ms);
            if (interval_ticks == 0) interval_ticks = pdMS_TO_TICKS(1000);
            if ((now - s_slaves[i].last_poll_tick) >= interval_ticks) {
                s_slaves[i].last_poll_tick = now;
                poll_slave(&s_slaves[i]);
            }
        }
        // Небольшая пауза, чтобы не жрать CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

/* --- Публичные API --- */

esp_err_t rs485_master_init(int baud, int rx_buf_size, int tx_buf_size)
{
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_baud = (baud > 0) ? baud : 9600;
    s_rx_buf = (rx_buf_size > 0) ? rx_buf_size : 1024;
    s_tx_buf = (tx_buf_size > 0) ? tx_buf_size : 512;

    // configure DE pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RS485_DE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(RS485_DE_PIN, 0);

    // init uart
    uart_config_t uart_conf = {
        .baud_rate = s_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if defined(UART_SCLK_DEFAULT)
        .source_clk = UART_SCLK_APB,
#endif
    };

    ESP_ERROR_CHECK(uart_param_config(RS485_UART_NUM, &uart_conf));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_NUM, RS485_TX_PIN, RS485_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // install driver without event queue (we read synchronously)
    esp_err_t r = uart_driver_install(RS485_UART_NUM, s_rx_buf, s_tx_buf, 0, NULL, 0);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(r));
        return r;
    }

     // --- diagnostic info ---
     ESP_LOGI(TAG, "uart_driver_install ok on uart %d", RS485_UART_NUM);
     ESP_LOGI(TAG, "uart pins: TX=%d RX=%d DE=%d", RS485_TX_PIN, RS485_RX_PIN, RS485_DE_PIN);

    s_data_mutex = xSemaphoreCreateMutex();
    s_uart_mutex = xSemaphoreCreateMutex();
    if (!s_data_mutex || !s_uart_mutex) {
        ESP_LOGE(TAG, "failed to create mutexes");
        return ESP_FAIL;
    }

    // clear slave table
    memset(s_slaves, 0, sizeof(s_slaves));
    s_slave_count = 0;
    s_running = true;

    // create poll task
    if (xTaskCreatePinnedToCore(poll_task, "rs485_poll", 4096, NULL, 6, &s_poll_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create poll task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "RS485 master init: baud=%d rx=%d tx=%d", s_baud, s_rx_buf, s_tx_buf);
    return ESP_OK;
}

int rs485_master_add_slave(const rs485_slave_cfg_t *cfg)
{
    if (!cfg) return -1;
    // find free slot
    for (int i = 0; i < RS485_MAX_SLAVES; ++i) {
        if (!s_slaves[i].in_use) {
            s_slaves[i].in_use = true;
            s_slaves[i].cfg = *cfg;
            s_slaves[i].data.slave_addr = cfg->slave_addr;
            s_slaves[i].data.reg_start = cfg->reg_start;
            s_slaves[i].data.reg_count = cfg->reg_count;
            s_slaves[i].last_poll_tick = xTaskGetTickCount() - pdMS_TO_TICKS(cfg->poll_interval_ms); // poll immediately
            s_slave_count++;
            ESP_LOGI(TAG, "added slave idx=%d addr=%d regs=%d @%" PRIu32 "ms",i, cfg->slave_addr, cfg->reg_count, cfg->poll_interval_ms);
            return i;
        }
    }
    return -1;
}

esp_err_t rs485_master_remove_slave(int index)
{
    if (index < 0 || index >= RS485_MAX_SLAVES) return ESP_ERR_INVALID_ARG;
    if (!s_slaves[index].in_use) return ESP_ERR_NOT_FOUND;
    s_slaves[index].in_use = false;
    s_slave_count--;
    return ESP_OK;
}

esp_err_t rs485_master_start(void)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_running = true;
    if (xTaskCreatePinnedToCore(poll_task, "rs485_poll", 4096, NULL, 6, &s_poll_task, 1) != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rs485_master_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    s_running = false;
    // wait for task finish
    // task will delete itself
    s_poll_task = NULL;
    return ESP_OK;
}

int rs485_master_get_slave_values(int index, uint16_t *out_buf, int buf_size_words, uint32_t *out_timestamp_ms, int *out_last_error)
{
    if (index < 0 || index >= RS485_MAX_SLAVES) return -1;
    if (!s_slaves[index].in_use) return -1;

    int count = s_slaves[index].cfg.reg_count;
    if (!out_buf || buf_size_words < count) return -1;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;
    }
    for (int i = 0; i < count; ++i) out_buf[i] = s_slaves[index].data.values[i];
    if (out_timestamp_ms) *out_timestamp_ms = s_slaves[index].data.timestamp_ms;
    if (out_last_error) *out_last_error = s_slaves[index].data.last_error;
    xSemaphoreGive(s_data_mutex);
    return count;
}

int rs485_master_get_count(void)
{
    return s_slave_count;
}

void rs485_master_deinit(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(10));
    // delete task if exists
    if (s_poll_task) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
    }
    // free driver
    uart_driver_delete(RS485_UART_NUM);
    if (s_data_mutex) { vSemaphoreDelete(s_data_mutex); s_data_mutex = NULL; }
    if (s_uart_mutex) { vSemaphoreDelete(s_uart_mutex); s_uart_mutex = NULL; }
    memset(s_slaves, 0, sizeof(s_slaves));
    s_slave_count = 0;
}



