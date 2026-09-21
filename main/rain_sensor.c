#include "rain_sensor.h"
#include "lushgate_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RAIN_SENSOR";
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_cali_enabled = false;
static uint8_t s_last_polarity = 0; // 0: A->B, 1: B->A

esp_err_t rain_sensor_init(void)
{
    // GPIO0 / GPIO1 をデフォルトで入力（Hi-Z、プルアップ/プルダウン無効）に設定
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_RAIN_SENSE_A) | (1ULL << PIN_RAIN_SENSE_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    if (s_adc_handle == NULL) {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = RAIN_ADC_UNIT,
        };
        esp_err_t err = adc_oneshot_new_unit(&init_config, &s_adc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
            return err;
        }

        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12, // 0 - ~3.1V 測定範囲
        };
        adc_oneshot_config_channel(s_adc_handle, RAIN_ADC_CH_A, &chan_config);
        adc_oneshot_config_channel(s_adc_handle, RAIN_ADC_CH_B, &chan_config);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = RAIN_ADC_UNIT,
            .chan = RAIN_ADC_CH_A,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali_handle) == ESP_OK) {
            s_cali_enabled = true;
        }
#endif
    }
    return ESP_OK;
}

void rain_sensor_power_down(void)
{
    // 両端子を完全Hi-Zにして待機時電位差・待機電流ゼロ化
    gpio_set_direction(PIN_RAIN_SENSE_A, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_RAIN_SENSE_B, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_RAIN_SENSE_A, GPIO_FLOATING);
    gpio_set_pull_mode(PIN_RAIN_SENSE_B, GPIO_FLOATING);
}

esp_err_t rain_sensor_read(uint16_t threshold_mv, rain_sensor_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    if (s_adc_handle == NULL) {
        esp_err_t err = rain_sensor_init();
        if (err != ESP_OK) return err;
    }

    // サンプリング毎に極性を反転
    s_last_polarity = (s_last_polarity == 0) ? 1 : 0;
    result->polarity = s_last_polarity;

    int raw_val = 0;
    int voltage_mv = 0;

    if (s_last_polarity == 0) {
        // 極性1: A (GPIO0) = HIGH, B (GPIO1) = ADC測定
        gpio_set_direction(PIN_RAIN_SENSE_A, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_RAIN_SENSE_A, 1);
        gpio_set_direction(PIN_RAIN_SENSE_B, GPIO_MODE_INPUT);

        esp_rom_delay_us(5000); // 5ms パルス印加待機
        adc_oneshot_read(s_adc_handle, RAIN_ADC_CH_B, &raw_val);
    } else {
        // 極性2: B (GPIO1) = HIGH, A (GPIO0) = ADC測定
        gpio_set_direction(PIN_RAIN_SENSE_B, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_RAIN_SENSE_B, 1);
        gpio_set_direction(PIN_RAIN_SENSE_A, GPIO_MODE_INPUT);

        esp_rom_delay_us(5000); // 5ms パルス印加待機
        adc_oneshot_read(s_adc_handle, RAIN_ADC_CH_A, &raw_val);
    }

    // 測定直後に即時Hi-Zに戻して電極腐食・電力浪費を遮断
    rain_sensor_power_down();

    if (s_cali_enabled && s_adc_cali_handle) {
        adc_cali_raw_to_voltage(s_adc_cali_handle, raw_val, &voltage_mv);
    } else {
        // 簡易換算 (12bit 3300mV)
        voltage_mv = (raw_val * 3300) / 4095;
    }

    result->raw_adc = (uint16_t)raw_val;
    result->voltage_mv = (uint16_t)voltage_mv;
    result->is_raining = (result->voltage_mv >= threshold_mv);

    ESP_LOGI(TAG, "Rain Sensor Read: Polarity=%d, Raw=%d, Volt=%dmV, IsRaining=%s (Thresh=%dmV)",
             result->polarity, result->raw_adc, result->voltage_mv,
             result->is_raining ? "YES" : "NO", threshold_mv);

    return ESP_OK;
}
