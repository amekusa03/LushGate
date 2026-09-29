#include "driver/uart.h"
#include "pump_control.h"
#include "lushgate_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
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
    gpio_hold_dis(PIN_PUMP_CTRL);
    gpio_set_level(PIN_PUMP_CTRL, off_level);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_PUMP_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = (off_level == 1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (off_level == 0) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Sleep中もOFFレベルを確実に保持するよう設定
    gpio_sleep_set_direction(PIN_PUMP_CTRL, GPIO_MODE_OUTPUT);
    gpio_sleep_set_pull_mode(PIN_PUMP_CTRL, (off_level == 1) ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    gpio_hold_en(PIN_PUMP_CTRL);

    pump_set_state(false);

    esp_timer_create_args_t timer_args = {
        .callback = &manual_stop_callback,
        .name = "pump_manual_timer"
    };
    esp_timer_create(&timer_args, &s_manual_timer);

    ESP_LOGI(TAG, "Pump initialized (Active Level: %d, Initial State: OFF, Sleep Hold Enabled)", s_active_level);
    return ESP_OK;
}

void pump_set_state(bool on)
{
    s_is_running = on;
    uint32_t level = on ? s_active_level : (1 - s_active_level);
    gpio_hold_dis(PIN_PUMP_CTRL);
    gpio_set_level(PIN_PUMP_CTRL, level);
    gpio_sleep_set_pull_mode(PIN_PUMP_CTRL, (level == 1) ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    gpio_hold_en(PIN_PUMP_CTRL);
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
    ESP_LOGI(TAG, "Starting power-saving watering cycle (Light Sleep enabled): ON=%ds, OFF=%ds, TotalON=%ds",
             on_sec, off_sec, total_on_sec);

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

        // =========================================================================
        // 1. Pump ON フェーズ (GPIO Hold でリレーを通電維持したまま Light Sleep 突入)
        // =========================================================================
        ESP_LOGI(TAG, "💧 [Watering] Pump ON for %d sec (Entering Light Sleep ~0.13mA)...", current_on_target);
        pump_set_state(true);
        fflush(stdout);
        esp_rom_delay_us(1000);

        // タイマー復帰 & ボタン緊急復帰設定
        esp_sleep_enable_timer_wakeup((uint64_t)current_on_target * 1000000ULL);
        gpio_wakeup_enable(PIN_USER_BUTTON, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        fflush(stdout);
        uart_wait_tx_idle_polling(0);

        esp_light_sleep_start();

        gpio_wakeup_disable(PIN_USER_BUTTON);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

        // スリープから目覚めた直後、安全のため即座にポンプを停止
        esp_sleep_wakeup_cause_t wake_reason = esp_sleep_get_wakeup_cause();
        pump_set_state(false);

        if (wake_reason == ESP_SLEEP_WAKEUP_GPIO || gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
            ESP_LOGW(TAG, "🔘 Watering interrupted by BOOT button!");
            return ESP_ERR_TIMEOUT;
        }

        accumulated_on_sec += current_on_target;
        ESP_LOGI(TAG, "💧 [Watering] Pump ON phase finished (Accumulated ON: %d/%ds)", accumulated_on_sec, total_on_sec);

        // 目標正味散水時間に達していれば完了
        if (accumulated_on_sec >= total_on_sec) {
            break;
        }

        // =========================================================================
        // 2. Pump OFF インターバルフェーズ (モーター冷却時間を Light Sleep で待機)
        // =========================================================================
        if (off_sec > 0) {
            ESP_LOGI(TAG, "⏸️ [Cooldown] Pump OFF for %d sec (Entering Light Sleep ~0.13mA)...", off_sec);
            fflush(stdout);
            esp_rom_delay_us(1000);

            esp_sleep_enable_timer_wakeup((uint64_t)off_sec * 1000000ULL);
            gpio_wakeup_enable(PIN_USER_BUTTON, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();

            fflush(stdout);
            uart_wait_tx_idle_polling(0);

            esp_light_sleep_start();

            gpio_wakeup_disable(PIN_USER_BUTTON);
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

            wake_reason = esp_sleep_get_wakeup_cause();
            if (wake_reason == ESP_SLEEP_WAKEUP_GPIO || gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
                ESP_LOGW(TAG, "🔘 Cooldown interrupted by BOOT button!");
                return ESP_ERR_TIMEOUT;
            }
        }
    }

    pump_set_state(false);
    ESP_LOGI(TAG, "✅ Watering cycle completed successfully. Total ON: %ds (Ultra Low Power Mode)", accumulated_on_sec);
    return ESP_OK;
}
