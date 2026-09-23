#include "pump_control.h"
#include "lushgate_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PUMP_CTRL";
static bool s_is_running = false;
static uint8_t s_active_level = PUMP_ACTIVE_LEVEL;
static esp_timer_handle_t s_manual_timer = NULL;

static void manual_stop_callback(void* arg)
{
    ESP_LOGI(TAG, "Manual pump test timeout reached, turning OFF pump");
    pump_manual_stop();
}

void pump_set_active_level(uint8_t active_level)
{
    s_active_level = active_level ? 1 : 0;
    ESP_LOGI(TAG, "Pump active level set to: %d (%s)", s_active_level, s_active_level ? "Active High" : "Active Low");
    pump_set_state(s_is_running);
}

esp_err_t pump_init(uint8_t active_level)
{
    s_active_level = active_level ? 1 : 0;
    
    // 安全のため、初期化前にピンをOFFレベルに設定してから出力設定
    uint32_t off_level = (1 - s_active_level);
    gpio_set_level(PIN_PUMP_CTRL, off_level);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_PUMP_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = (off_level == 1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (off_level == 0) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    pump_set_state(false);

    esp_timer_create_args_t timer_args = {
        .callback = &manual_stop_callback,
        .name = "pump_manual_timer"
    };
    esp_timer_create(&timer_args, &s_manual_timer);

    ESP_LOGI(TAG, "Pump initialized (Active Level: %d, Initial State: OFF)", s_active_level);
    return ESP_OK;
}

void pump_set_state(bool on)
{
    s_is_running = on;
    uint32_t level = on ? s_active_level : (1 - s_active_level);
    gpio_set_level(PIN_PUMP_CTRL, level);
    ESP_LOGI(TAG, "Pump state -> %s (GPIO%d = %lu)", on ? "ON" : "OFF", PIN_PUMP_CTRL, (unsigned long)level);
}

bool pump_is_running(void)
{
    return s_is_running;
}

esp_err_t pump_manual_start(uint16_t duration_sec)
{
    if (duration_sec == 0 || duration_sec > 1800) {
        duration_sec = 30; // 安全デフォルト 30秒
    }
    pump_set_state(true);

    if (s_manual_timer) {
        esp_timer_stop(s_manual_timer);
        esp_timer_start_once(s_manual_timer, (uint64_t)duration_sec * 1000000ULL);
    }
    ESP_LOGI(TAG, "Manual pump test started for %d seconds", duration_sec);
    return ESP_OK;
}

esp_err_t pump_manual_stop(void)
{
    if (s_manual_timer) {
        esp_timer_stop(s_manual_timer);
    }
    pump_set_state(false);
    ESP_LOGI(TAG, "Manual pump test stopped");
    return ESP_OK;
}

esp_err_t pump_run_watering_cycle(uint16_t on_sec, uint16_t off_sec, uint16_t total_on_sec, volatile bool *abort_requested)
{
    if (on_sec == 0 || total_on_sec == 0) return ESP_ERR_INVALID_ARG;

    uint16_t accumulated_on_sec = 0;
    ESP_LOGI(TAG, "Starting watering cycle: ON=%ds, OFF=%ds, TotalON=%ds", on_sec, off_sec, total_on_sec);

    while (accumulated_on_sec < total_on_sec) {
        if (abort_requested && *abort_requested) {
            ESP_LOGW(TAG, "Watering cycle aborted by request");
            pump_set_state(false);
            return ESP_ERR_TIMEOUT;
        }

        uint16_t current_on_target = on_sec;
        if (accumulated_on_sec + current_on_target > total_on_sec) {
            current_on_target = total_on_sec - accumulated_on_sec;
        }

        // Pump ON phase
        pump_set_state(true);
        for (uint16_t s = 0; s < current_on_target; s++) {
            if (abort_requested && *abort_requested) {
                pump_set_state(false);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            accumulated_on_sec++;
        }
        pump_set_state(false);

        // Check if finished
        if (accumulated_on_sec >= total_on_sec) {
            break;
        }

        // Pump OFF interval phase
        ESP_LOGI(TAG, "Duty cycle: Cooling down for %d seconds (Accumulated ON: %d/%ds)",
                 off_sec, accumulated_on_sec, total_on_sec);
        for (uint16_t s = 0; s < off_sec; s++) {
            if (abort_requested && *abort_requested) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    pump_set_state(false);
    ESP_LOGI(TAG, "Watering cycle completed successfully. Total ON: %ds", accumulated_on_sec);
    return ESP_OK;
}
