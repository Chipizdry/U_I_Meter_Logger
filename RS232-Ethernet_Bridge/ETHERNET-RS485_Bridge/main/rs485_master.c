











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
#include "esp_heap_caps.h"

#include "websocket_client.h"
#include "nvs_settings.h" 
#include "gpio_manager.h"

static const char *TAG = "rs485_master";
static esp_timer_handle_t s_de_off_timer = NULL;
static void rs485_request_task(void *arg);
static void de_off_timer_cb(void *arg);
QueueHandle_t s_req_queue = NULL;
static TaskHandle_t s_req_task = NULL;


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
/*
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
*/

static void de_off_timer_cb(void *arg)
{
    rs485_set_de(0);
    ESP_LOGD(TAG, "DE → 0 (timer expired)");
}

/* Управление DE (direction) */
void rs485_set_de(int level)
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
/*
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
*/
/* Чтение ответа с таймаутом: читаем по одному байту, пока не наберём max или не выйдет таймаут */
   

int rs485_read_available(uint8_t *buf, int max, int timeout_ms) {
    int len = 0;
    int total = 0;
    int t = 0;
    while (t < timeout_ms && total < max) {
        len = uart_read_bytes(RS485_UART_NUM, buf + total, 1, pdMS_TO_TICKS(10));
        if (len > 0) {
            total += len;
            t = 0;  // сбрасываем таймер, если что-то пришло
        } else {
            t += 10;
        }
    }
    return total;
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


      // Перезапускаем глобальный таймер
      esp_timer_stop(s_de_off_timer);
      esp_timer_start_once(s_de_off_timer, total_time_us);

  

    //ESP_LOGI(TAG, "TX started (%d bytes), DE→1 for ~%" PRIu64 " us", req_len, total_time_us);
   // ESP_LOG_BUFFER_HEX(TAG, req, req_len);

    xSemaphoreGive(s_uart_mutex);
    return ESP_OK;
}

/* Single poll operation on a slave: отправка запроса и чтение ответа, update data */
/*
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

    int r = rs485_read_available(resp, expected_len + 5, timeout_ms);
    ESP_LOGI(TAG, "RX raw (%d bytes):", r);
    ESP_LOG_BUFFER_HEX(TAG, resp, r);

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
*/
/* Polling task: идёт по всем слейвам и опрашивает те, у которых время пришло */
/*
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
*/
/* --- Публичные API --- */

esp_err_t rs485_master_init_from_cfg(const uart_settings_t *cfg, int rx_buf_size, int tx_buf_size)
{
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_baud = (cfg && cfg->baud_rate > 0) ? cfg->baud_rate : 9600;
    s_rx_buf = (rx_buf_size > 0) ? rx_buf_size : 1024;
    s_tx_buf = (tx_buf_size > 0) ? tx_buf_size : 512;

    gpio_set_level(RS485_DE_PIN, 0);

      // Устанавливаем уровень в зависимости от режима
    gpio_set_level(GPIO_MODE_CHANGE, cfg && cfg->rs485_mode ? 1 : 0);
    // Конфигурация UART
    uart_config_t uart_conf = {
        .baud_rate = (cfg) ? cfg->baud_rate : 9600,                  // скорость
        .data_bits = (cfg) ? cfg->data_bits : UART_DATA_8_BITS,      // 5..8 бит
        .parity    = (cfg) ? cfg->parity : UART_PARITY_DISABLE,      // none, odd, even
        .stop_bits = (cfg) ? cfg->stop_bits : UART_STOP_BITS_1,      // 1, 1.5, 2
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,                       // flow control не используем
        
    #if defined(UART_SCLK_DEFAULT)
        .source_clk = UART_SCLK_APB,                                 // источник тактирования
    #endif
    };

    ESP_ERROR_CHECK(uart_param_config(RS485_UART_NUM, &uart_conf));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_NUM, RS485_TX_PIN, RS485_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    const esp_timer_create_args_t targs = {
        .callback = &de_off_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "de_off_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_de_off_timer));

    // Установка драйвера UART
    esp_err_t r = uart_driver_install(RS485_UART_NUM, s_rx_buf, s_tx_buf, 0, NULL, 0);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(r));
        return r;
    }

    ESP_LOGI(TAG, "uart_driver_install ok on uart %d", RS485_UART_NUM);
    ESP_LOGI(TAG, "uart pins: TX=%d RX=%d DE=%d", RS485_TX_PIN, RS485_RX_PIN, RS485_DE_PIN);

    s_data_mutex = xSemaphoreCreateMutex();
    s_uart_mutex = xSemaphoreCreateMutex();
    if (!s_data_mutex || !s_uart_mutex) {
        ESP_LOGE(TAG, "failed to create mutexes");
        return ESP_FAIL;
    }

    memset(s_slaves, 0, sizeof(s_slaves));
    s_slave_count = 0;
    s_running = true;

    s_req_queue = xQueueCreate(5, sizeof(rs485_req_t));
  
    xTaskCreatePinnedToCore( rs485_request_task,"rs485_req_task",4096,NULL, 7,&s_req_task, 1);


    ESP_LOGI(TAG, "RS485 master init: baud=%d rx=%d tx=%d mode=%s",s_baud, s_rx_buf, s_tx_buf,(cfg && cfg->rs485_mode) ? "RS485" : "RS232");
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

    // удалить запросную задачу
    if (s_req_task) {
        vTaskDelete(s_req_task);
        s_req_task = NULL;
    }

    // удалить очередь
    if (s_req_queue) {
        vQueueDelete(s_req_queue);
        s_req_queue = NULL;
    }

    // удалить poll task
    if (s_poll_task) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
    }

    uart_driver_delete(RS485_UART_NUM);

    if (s_de_off_timer) { esp_timer_delete(s_de_off_timer); s_de_off_timer = NULL; }
    if (s_data_mutex) { vSemaphoreDelete(s_data_mutex); s_data_mutex = NULL; }
    if (s_uart_mutex) { vSemaphoreDelete(s_uart_mutex); s_uart_mutex = NULL; }

    memset(s_slaves, 0, sizeof(s_slaves));
    s_slave_count = 0;
}





esp_err_t rs485_master_send_raw(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t max_rx_len, int *out_rx_len, int timeout_ms)
{
    if (!tx || tx_len == 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;

    rs485_set_de(1);
   // uart_flush(RS485_UART_NUM);
    uart_flush_input(RS485_UART_NUM); 
    int written = uart_write_bytes(RS485_UART_NUM, (const char *)tx, tx_len);
    uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(100));

    rs485_set_de(0);

    if (written != tx_len) {
        xSemaphoreGive(s_uart_mutex);
        return ESP_FAIL;
    }

    int len = uart_read_bytes(RS485_UART_NUM, rx, max_rx_len, pdMS_TO_TICKS(timeout_ms));
    *out_rx_len = len;

    xSemaphoreGive(s_uart_mutex);
    return (len > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}


esp_err_t rs485_master_send(const uint8_t *data, size_t len)
{
    if (!s_req_queue) return ESP_FAIL;
    rs485_req_t req = {0};
    if (len > sizeof(req.data)) return ESP_ERR_INVALID_SIZE;
    memcpy(req.data, data, len);
    req.len = len;
    return xQueueSend(s_req_queue, &req, 10) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void rs485_request_task(void *arg)
{
    rs485_req_t req;
    const int uart_num = RS485_UART_NUM;

    while (s_running) {
        char ws_msg[512];          // <── создаётся один раз на итерацию, можно переиспользовать
        char hex_resp[256];        // <── тоже тут

        if (xQueueReceive(s_req_queue, &req, portMAX_DELAY) == pdTRUE) {

         //   ESP_LOGI(TAG, "Dequeued custom RS485 request (%d bytes)", req.len);
         // size_t heap_before = esp_get_free_heap_size();

         //   ESP_LOGI(TAG, "RS485 request start, free heap=%u",(unsigned)heap_before);

            uint32_t char_time_us = char_time_us_from_baud(s_baud);
            send_request_async(uart_num, req.data, req.len, char_time_us);

            uint8_t resp[256] = {0};
            int rx_len = uart_read_bytes(uart_num, resp, sizeof(resp), pdMS_TO_TICKS(1000));
           //  size_t heap_after = esp_get_free_heap_size();

          //  ESP_LOGI(TAG, "RS485 request end: RX=%d heap=%u delta=%d", rx_len,(unsigned)heap_after,(int)heap_after - (int)heap_before);

            if (rx_len > 0) {

              //  ESP_LOGI(TAG, "Received %d bytes from RS485", rx_len);
               // ESP_LOG_BUFFER_HEX(TAG, resp, rx_len);

                bytes_to_hex(resp, rx_len, hex_resp, sizeof(hex_resp));

                // --- переиспользуем ws_msg здесь ---
            snprintf(ws_msg, sizeof(ws_msg),
                    "{\"cmd\":\"%s\",\"hex_response\":\"%s\",\"command_name\":\"%s\"}",
                    req.cmd[0] ? req.cmd : "UNKNOWN",
                    hex_resp,
                    req.command_name[0] ? req.command_name : "UNKNOWN");
            websocket_send_text(ws_msg);
             //   ESP_LOGI(TAG, "Sent response back to WebSocket: %s", ws_msg);

            } else {
                ESP_LOGW(TAG, "No response from slave");  
                // --- переиспользуем ws_msg снова ---
                snprintf(ws_msg, sizeof(ws_msg), "{\"hex_response\":\"No response from RS485\"}");
               websocket_send_text(ws_msg);
          
            }
        }
    }
    vTaskDelete(NULL);
}


esp_err_t rs485_master_send_req(const rs485_req_t *req)
{
    if (!s_req_queue || !req) return ESP_FAIL;
    return xQueueSend(s_req_queue, req, pdMS_TO_TICKS(10)) == pdTRUE
           ? ESP_OK
           : ESP_ERR_TIMEOUT;
}

