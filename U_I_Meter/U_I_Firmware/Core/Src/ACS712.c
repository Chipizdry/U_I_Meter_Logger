



#include "ACS712.h"

/* Хранение откалиброванного нуля */
static int32_t adc_zero_offset = 2050;


/**
 * @brief Калибровка нуля
 */
void ACS712_CalibrateZero(uint16_t raw_adc_value)
{
    adc_zero_offset = (int32_t)raw_adc_value;
}


/**
 * @brief Преобразование ADC -> ток (мА)
 */
int16_t ACS712_GetCurrent_mA(uint16_t adc_raw)
{

	  if (adc_raw < 100 || adc_raw > 4000)
	        return 0;

    /* 1. Отклонение от нуля */
    int32_t delta = (int32_t)adc_raw - adc_zero_offset;

    /* 2. Перевод в мА */
    float current_f = (float)delta * ACS712_MA_PER_STEP;

    /* 3. Ограничение диапазона int16 */

    /*
    if (current_f > 32767.0f)
        current_f = 32767.0f;

    if (current_f < -32768.0f)
        current_f = -32768.0f;
     */
    return (int16_t)current_f;
}


/**
 * @brief Получить значение для Modbus регистра
 *
 * Modbus передаёт 16 бит.
 * Отрицательные числа автоматически будут переданы в 2's complement.
 */
uint16_t ACS712_GetCurrent_Modbus(uint16_t adc_raw)
{
    int16_t current = ACS712_GetCurrent_mA(adc_raw);
    return (uint16_t)current;
}



