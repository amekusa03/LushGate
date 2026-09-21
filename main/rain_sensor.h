#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint16_t raw_adc;       // 生ADC読み取り値
    uint16_t voltage_mv;    // 電圧 (mV)
    bool     is_raining;    // 閾値以上の雨滴検知フラグ
    uint8_t  polarity;      // 0: A->B, 1: B->A
} rain_sensor_result_t;

esp_err_t rain_sensor_init(void);
esp_err_t rain_sensor_read(uint16_t threshold_mv, rain_sensor_result_t *result);
void rain_sensor_power_down(void);
