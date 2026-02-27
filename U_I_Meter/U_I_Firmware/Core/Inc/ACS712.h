


#ifndef __ACS712_H__
#define __ACS712_H__

#include <stdint.h>

/* ===================== НАСТРОЙКИ ===================== */

/* Напряжение опоры АЦП (мВ) */
#define ACS712_ADC_VREF_MV      3300.0f

/* Разрешение АЦП (12 бит) */
#define ACS712_ADC_RESOLUTION   4096.0f

/* Чувствительность ACS712 30A (мВ/А) */
#define ACS712_SENSITIVITY_MVA  66.0f

/* Ваш коэффициент делителя (1.65 / 2.5) */
#define ACS712_DIVIDER_RATIO    0.66f

/* ===================================================== */

/*
 * Рассчитанный коэффициент:
 * сколько миллиампер соответствует одному шагу АЦП
 *
 * MA_PER_STEP = (1 / ((SENSITIVITY * DIVIDER) / (VREF / ADC_RES))) * 1000
 *
 * Для ваших параметров:
 * ≈ 18.495 мА на один шаг АЦП
 */
#define ACS712_MA_PER_STEP      18.495f


/* ===================== API ===================== */

/**
 * @brief Калибровка нуля (вызывать при 0А)
 * @param raw_adc_value значение АЦП
 */
void ACS712_CalibrateZero(uint16_t raw_adc_value);

/**
 * @brief Получить ток в миллиамперах
 * @param adc_raw значение АЦП
 * @return ток в мА (int16_t)
 */
int16_t ACS712_GetCurrent_mA(uint16_t adc_raw);

/**
 * @brief Получить ток для Modbus (int16_t приведённый к uint16_t)
 * @param adc_raw значение АЦП
 * @return значение для Modbus регистра
 */
uint16_t ACS712_GetCurrent_Modbus(uint16_t adc_raw);

#endif

