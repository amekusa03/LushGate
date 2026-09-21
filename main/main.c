#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

#include "lushgate_pins.h"
#include "storage_manager.h"
#include "rain_sensor.h"
#include "pump_control.h"
#include "wifi_ap.h"
#include "web_server.h"

static const char *TAG = "LUSHGATE_MAIN";

// Deep Sleep 中も保持される RTC メモリ変数
RTC_DATA_ATTR static uint16_t rtc_accumulated_rain_min = 0;
RTC_DATA_ATTR static uint8_t  rtc_last_water_day = 0xFF; // 同日重複散水防止用
RTC_DATA_ATTR static uint32_t rtc_boot_count = 0;

static void init_hardware(void)
{
    // LED設定
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << PIN_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL); // 消灯

    // BOOTボタン設定
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_USER_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    rain_sensor_init();
    pump_init();
}

static bool check_ap_mode_requested(void)
{
    // 起動時にボタンが押されているか判定 (長押し 1.5〜2秒 検知)
    if (gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
        ESP_LOGI(TAG, "BOOT button pressed, waiting for hold verification...");
        for (int i = 0; i < 20; i++) { // 2.0秒待機
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(PIN_USER_BUTTON) != BUTTON_PRESSED_LEVEL) {
                return false;
            }
        }
        ESP_LOGI(TAG, "BOOT button held for 2s: Entering AP Maintenance Mode");
        return true;
    }
    return false;
}

static void run_ap_maintenance_mode(const lushgate_config_t *cfg)
{
    ESP_LOGI(TAG, "=== LushGate AP Maintenance Mode Started ===");

    g_web_state.current_rain_accum_min = rtc_accumulated_rain_min;
    g_web_state.request_sleep = false;

    // Wi-Fi AP & mDNS & Web Server 起動
    wifi_ap_start(cfg->ap_ssid, cfg->ap_pass);
    web_server_start();

    uint32_t elapsed_sec = 0;
    const uint32_t timeout_sec = (cfg->ap_timeout_sec > 0) ? cfg->ap_timeout_sec : 300;

    // メインループ: LED点滅 & タイムアウト監視
    while (!g_web_state.request_sleep && (elapsed_sec < timeout_sec)) {
        gpio_set_level(PIN_STATUS_LED, LED_ACTIVE_LEVEL); // 点灯
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL); // 消灯
        vTaskDelay(pdMS_TO_TICKS(900));

        elapsed_sec++;
        if (elapsed_sec % 30 == 0) {
            ESP_LOGI(TAG, "AP Mode active: %lu / %lu sec", (unsigned long)elapsed_sec, (unsigned long)timeout_sec);
        }
    }

    ESP_LOGI(TAG, "Exiting AP Maintenance Mode, stopping servers...");
    web_server_stop();
    wifi_ap_stop();
}

static void run_normal_monitoring_cycle(const lushgate_config_t *cfg)
{
    rtc_boot_count++;
    ESP_LOGI(TAG, "=== Normal Monitoring Wakeup #%lu ===", (unsigned long)rtc_boot_count);

    // 1. 雨センサー測定
    rain_sensor_result_t rain_res = {0};
    rain_sensor_read(cfg->adc_thresh_mv, &rain_res);

    if (rain_res.is_raining) {
        uint16_t add_min = (cfg->sleep_interval_sec >= 60) ? (cfg->sleep_interval_sec / 60) : 1;
        rtc_accumulated_rain_min += add_min;
        ESP_LOGI(TAG, "Rain detected! Added %d min. 24h Accum: %d min", add_min, rtc_accumulated_rain_min);
    } else {
        ESP_LOGI(TAG, "No rain detected. 24h Accum: %d min", rtc_accumulated_rain_min);
    }

    // 2. 時刻・散水スケジュール判定
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    bool is_sched_time = (timeinfo.tm_hour == cfg->sched_hour) &&
                         (timeinfo.tm_min >= cfg->sched_min && timeinfo.tm_min < (cfg->sched_min + 15));

    if (is_sched_time && (rtc_last_water_day != timeinfo.tm_mday)) {
        ESP_LOGI(TAG, "Schedule reached! (Hour: %d, Min: %d) Checking rain accumulation...",
                 timeinfo.tm_hour, timeinfo.tm_min);

        water_history_entry_t entry = {
            .timestamp = (uint32_t)now,
            .rain_accum_min = rtc_accumulated_rain_min,
            .battery_mv = 0,
        };

        if (rtc_accumulated_rain_min >= cfg->rain_thresh_min) {
            ESP_LOGI(TAG, "Sufficient rain (%d min >= %d min), SKIPPING watering today.",
                     rtc_accumulated_rain_min, cfg->rain_thresh_min);
            entry.result = WATER_RESULT_SKIPPED_RAIN;
            entry.pump_run_sec = 0;
        } else {
            ESP_LOGI(TAG, "Insufficient rain (%d min < %d min), STARTING watering sequence (%d sec)!",
                     rtc_accumulated_rain_min, cfg->rain_thresh_min, cfg->pump_total_sec);
            
            // ポンプデューティ散水実行
            pump_run_watering_cycle(cfg->pump_on_sec, cfg->pump_off_sec, cfg->pump_total_sec, NULL);
            entry.result = WATER_RESULT_WATERED;
            entry.pump_run_sec = cfg->pump_total_sec;
        }

        // 履歴をNVSに永続保存
        storage_add_history(&entry);

        // カウンタと本日散水フラグの更新
        rtc_last_water_day = timeinfo.tm_mday;
        rtc_accumulated_rain_min = 0;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "LushGate Starting Up...");

    storage_init();

    lushgate_config_t config;
    storage_load_config(&config);

    init_hardware();

    // APモード要求の確認
    if (check_ap_mode_requested()) {
        run_ap_maintenance_mode(&config);
    } else {
        run_normal_monitoring_cycle(&config);
    }

    // 雨センサー・ポンプの待機状態確認
    rain_sensor_power_down();
    pump_set_state(false);

    // Deep Sleep 設定 (ボタン押下での復帰 + タイマー復帰)
    uint64_t sleep_us = (uint64_t)config.sleep_interval_sec * 1000000ULL;
    if (sleep_us < 10000000ULL) sleep_us = 180ULL * 1000000ULL; // 安全マージン (最低10秒)

    ESP_LOGI(TAG, "Entering Deep Sleep for %d seconds. (Wakeup on Timer or GPIO9)", config.sleep_interval_sec);
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_USER_BUTTON), ESP_GPIO_WAKEUP_GPIO_LOW);

    esp_deep_sleep_start();
}
