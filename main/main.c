#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"

#include "lushgate_pins.h"
#include "storage_manager.h"
#include "rain_sensor.h"
#include "pump_control.h"
#include "wifi_ap.h"
#include "web_server.h"

static const char *TAG = "LUSHGATE_MAIN";

// 状態変数 (Light Sleep 中は RAM がそのまま保持される)
static uint16_t s_accumulated_rain_min = 0;
static uint8_t  s_last_water_day = 0xFF; // 同日重複散水防止用
static uint32_t s_cycle_count = 0;
static uint32_t s_last_nvs_backup_time = 0;

static void init_hardware(const lushgate_config_t *cfg)
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

    // BOOTボタン設定 (GPIO9)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_USER_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    rain_sensor_init();
    pump_init(cfg ? cfg->pump_active_level : 0);
}

// 起動時にボタン受付ウィンドウを設ける（LED点滅中に押下検知）
static bool check_boot_window_button(void)
{
    ESP_LOGI(TAG, "Checking BOOT button window (Press BOOT within 2 seconds for AP Mode)...");

    for (int i = 0; i < 20; i++) { // 2秒間 (100ms * 20)
        gpio_set_level(PIN_STATUS_LED, (i % 2 == 0) ? LED_ACTIVE_LEVEL : (1 - LED_ACTIVE_LEVEL));

        if (gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
            ESP_LOGI(TAG, "BOOT button pressed! Entering AP Maintenance Mode...");
            gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL);
    return false;
}

static void run_ap_maintenance_mode(const lushgate_config_t *cfg)
{
    ESP_LOGI(TAG, "=== LushGate AP Maintenance Mode Started ===");

    g_web_state.current_rain_accum_min = s_accumulated_rain_min;
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
        if (elapsed_sec % 10 == 0) {
            ESP_LOGI(TAG, "AP Mode active: %lu / %lu sec (SSID: %s, IP: 192.168.4.1)",
                     (unsigned long)elapsed_sec, (unsigned long)timeout_sec, cfg->ap_ssid);
        }
    }

    ESP_LOGI(TAG, "Exiting AP Maintenance Mode, stopping servers...");
    web_server_stop();
    wifi_ap_stop();
}

static void run_normal_monitoring_cycle(const lushgate_config_t *cfg)
{
    s_cycle_count++;
    ESP_LOGI(TAG, "=== Normal Monitoring Cycle #%lu ===", (unsigned long)s_cycle_count);

    // 1. 雨センサー測定 (パルス通電・超低消費電力)
    rain_sensor_result_t rain_res = {0};
    rain_sensor_read(cfg->adc_thresh_mv, &rain_res);

    if (rain_res.is_raining) {
        uint16_t add_min = (cfg->sleep_interval_sec >= 60) ? (cfg->sleep_interval_sec / 60) : 1;
        s_accumulated_rain_min += add_min;
        ESP_LOGI(TAG, "🌧️ Rain detected! Added %d min. 24h Accum: %d min", add_min, s_accumulated_rain_min);
    } else {
        ESP_LOGI(TAG, "☀️ No rain detected. 24h Accum: %d min", s_accumulated_rain_min);
    }

    // 2. 時刻・散水スケジュール判定
    time_t now = time(NULL);
    if (now < 1700000000) {
        ESP_LOGW(TAG, "RTC time is not synchronized (Epoch: %ld). Skipping scheduled watering.", (long)now);
        return;
    }
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // 散水時刻ウィンドウ判定 (指定時刻から15分以内)
    bool is_sched_time = (timeinfo.tm_hour == cfg->sched_hour) &&
                         (timeinfo.tm_min >= cfg->sched_min && timeinfo.tm_min < (cfg->sched_min + 15));

    if (is_sched_time && (s_last_water_day != timeinfo.tm_mday)) {
        ESP_LOGI(TAG, "⏰ Schedule reached! (%02d:%02d) Checking rain accumulation...",
                 timeinfo.tm_hour, timeinfo.tm_min);

        water_history_entry_t entry = {
            .timestamp = (uint32_t)now,
            .rain_accum_min = s_accumulated_rain_min,
            .battery_mv = 0,
        };

        if (s_accumulated_rain_min >= cfg->rain_thresh_min) {
            ESP_LOGI(TAG, "💧 Sufficient rain (%d min >= %d min), SKIPPING watering today.",
                     s_accumulated_rain_min, cfg->rain_thresh_min);
            entry.result = WATER_RESULT_SKIPPED_RAIN;
            entry.pump_run_sec = 0;
        } else {
            ESP_LOGI(TAG, "🌱 Insufficient rain (%d min < %d min), STARTING watering sequence (%d sec)!",
                     s_accumulated_rain_min, cfg->rain_thresh_min, cfg->pump_total_sec);
            
            // ポンプデューティ散水実行
            pump_run_watering_cycle(cfg->pump_on_sec, cfg->pump_off_sec, cfg->pump_total_sec, NULL);
            entry.result = WATER_RESULT_WATERED;
            entry.pump_run_sec = cfg->pump_total_sec;
        }

        // 履歴をNVSに永続保存
        storage_add_history(&entry);

        // カウンタと本日散水フラグの更新
        s_last_water_day = timeinfo.tm_mday;
        s_accumulated_rain_min = 0;

        // 散水完了ステータスを即座にNVSバックアップ
        storage_save_last_state((uint32_t)now, s_accumulated_rain_min, s_last_water_day);
        s_last_nvs_backup_time = (uint32_t)now;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "    LushGate Starting Up               ");
    ESP_LOGI(TAG, "=======================================");

    // 1. ストレージ初期化 & 設定読み出し
    storage_init();
    lushgate_config_t config;
    storage_load_config(&config);

    // 2. ハードウェア初期化
    init_hardware(&config);

    // 3. リセット時・電源投入時の時刻＆雨量バックアップ復旧
    time_t now = time(NULL);
    if (now < 1700000000) {
        uint32_t saved_time = 0;
        uint16_t saved_rain = 0;
        uint8_t saved_water_day = 0xFF;
        if (storage_load_last_state(&saved_time, &saved_rain, &saved_water_day) == ESP_OK && saved_time >= 1700000000) {
            struct timeval tv = { .tv_sec = (time_t)saved_time, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            s_accumulated_rain_min = saved_rain;
            s_last_water_day = saved_water_day;
            s_last_nvs_backup_time = saved_time;
            ESP_LOGI(TAG, "✅ Restored RTC Time & Rain from NVS! (Epoch: %lu, Rain: %d min, LastDay: %d)",
                     (unsigned long)saved_time, s_accumulated_rain_min, s_last_water_day);
        } else {
            ESP_LOGW(TAG, "⚠️ No valid backup state found in NVS. Waiting for smartphone time sync.");
        }
    }

    // 4. 初回起動時のボタン受付ウィンドウ (2秒間LED点滅)
    bool enter_ap = false;
    if (gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL || check_boot_window_button()) {
        enter_ap = true;
    }

    while (1) {
        if (enter_ap) {
            // APモード（設定画面）の実行
            run_ap_maintenance_mode(&config);
            enter_ap = false;

            // 設定画面から抜けた直後、最新の時刻・雨量を即座にNVSバックアップ
            now = time(NULL);
            if (now >= 1700000000) {
                storage_save_last_state((uint32_t)now, s_accumulated_rain_min, s_last_water_day);
                s_last_nvs_backup_time = (uint32_t)now;
            }
        }

        // 通常雨量測定＆散水判定
        run_normal_monitoring_cycle(&config);

        // 周辺回路の完全待機化 (省電力＆腐食防止)
        rain_sensor_power_down();
        pump_set_state(false);

        // NVSへのスマートバックアップ (15分おき、または雨量変化時)
        now = time(NULL);
        if (now >= 1700000000) {
            if ((now - s_last_nvs_backup_time >= 900) || (s_accumulated_rain_min > 0 && (now - s_last_nvs_backup_time >= 180))) {
                storage_save_last_state((uint32_t)now, s_accumulated_rain_min, s_last_water_day);
                s_last_nvs_backup_time = (uint32_t)now;
            }
        }

        // --- Light Sleep の設定 ---
        // A. タイマー復帰 (デフォルト180秒 = 3分)
        uint64_t sleep_us = (uint64_t)config.sleep_interval_sec * 1000000ULL;
        if (sleep_us < 5000000ULL) sleep_us = 180ULL * 1000000ULL;
        esp_sleep_enable_timer_wakeup(sleep_us);

        // B. BOOTボタン (GPIO9 LOW) による即時復帰
        gpio_wakeup_enable(PIN_USER_BUTTON, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        ESP_LOGI(TAG, "Entering Light Sleep (%d sec)... [Press BOOT button anytime for AP Mode]", config.sleep_interval_sec);
        fflush(stdout);
        esp_rom_delay_us(1000);

        // Light Sleep 突入 (RAM保持、RTC発振継続、消費電流 約0.13mA)
        esp_light_sleep_start();

        // --- 目覚めた後の処理 ---
        esp_sleep_wakeup_cause_t wake_reason = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Woke up from Light Sleep, reason: %d", wake_reason);

        // ボタン押下で起きた場合は次回ループで即座にAPモードを起動
        if (wake_reason == ESP_SLEEP_WAKEUP_GPIO || gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
            ESP_LOGI(TAG, "🔘 Woken up by BOOT button! Entering AP Maintenance Mode...");
            vTaskDelay(pdMS_TO_TICKS(200)); // チャタリング防止
            enter_ap = true;
        }
    }
}
