







#ifndef RS485_MASTER_H
#define RS485_MASTER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485_UART_NUM        2              // UART2
#define RS485_TX_PIN          17
#define RS485_RX_PIN          35
#define RS485_DE_PIN          33

// Максимальное количество опрашиваемых устройств
#define RS485_MAX_SLAVES      32

typedef struct {
    uint8_t slave_addr;        // Modbus address
    uint16_t reg_start;        // адрес первого регистра (Modbus: 0-based)
    uint16_t reg_count;        // количество регистров
    uint32_t poll_interval_ms; // интервал опроса
} rs485_slave_cfg_t;

typedef struct {
    uint8_t slave_addr;
    uint16_t reg_start;
    uint16_t reg_count;
    uint16_t values[64];      // NOTE: ограничение, можно увеличить при необходимости
    uint32_t timestamp_ms;    // время последнего успешного обновления (xTaskGetTickCount()*portTICK_PERIOD_MS)
    int last_error;           // код последней ошибки (0 == OK)
} rs485_slave_data_t;

// Инициализация модуля RS485 master:
//  baud - скорость (например 9600, 57600)
//  rx_buf_size, tx_buf_size - размеры аппаратных буферов (байты)
esp_err_t rs485_master_init(int baud, int rx_buf_size, int tx_buf_size);

// Добавить слейв в таблицу (возвращает индекс или -1)
int rs485_master_add_slave(const rs485_slave_cfg_t *cfg);

// Удалить слейв по индексу (0..count-1)
esp_err_t rs485_master_remove_slave(int index);

// Запустить/остановить опрос (task создаётся при инициализации автоматически, но можно остановить)
esp_err_t rs485_master_start(void);
esp_err_t rs485_master_stop(void);

// Скопировать snapshot данных слейва (thread-safe).
// buf_size_words - размер буфера в uint16_t, возвращает количество записанных регистров или -1 при ошибке
int rs485_master_get_slave_values(int index, uint16_t *out_buf, int buf_size_words, uint32_t *out_timestamp_ms, int *out_last_error);

// Получить текущее число слейвов
int rs485_master_get_count(void);

// Деинициализация (удаляет таски, драйвер)
void rs485_master_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // RS485_MASTER_H


