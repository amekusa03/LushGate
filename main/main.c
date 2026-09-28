/**
 * @file main.c
 * @brief LushGate - ソーラー＆乾電池駆動 自動灌水コントローラー (BLE専用・堅牢版)
 *
 * 【動作フロー】
 *  1. 起動時・リセット時にNVSおよびRTCスローメモリから設定と状態を復元
 *  2. 時刻未設定時、またはBOOTボタン押下時は「BLEメンテナンスモード」起動 (5分間)
 *     - Web Bluetooth アプリから時刻同期、設定編集、散水手動テスト、履歴閲覧が可能
 *  3. 通常運用時：
 *     - 周期的に雨センサーをパルス通電測定 (超低消費電力 & 電極腐食防止)
 *     - 降雨があれば雨量積算、不足していれば設定時刻にポンプ散水実行 (1日1回厳格制御)
 *     - 状態を定期的にNVSスマートバックアップ
 *     - 次回測定まで Light Sleep (RAM保持・RTC発振・消費電流 約0.13mA)
 *     - スリープ中はいつでもBOOTボタン押下でBLEモードに即時復帰
 */

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
#include "ble_gatt.h"

static const char *TAG = "LushGate";

/* 前方宣言 */
static void run_normal_monitoring_cycle(const lushgate_config_t *cfg);

/* グローバル状態変数 (RTCスローメモリ保持: Sleep/Reset後も維持) */
static RTC_DATA_ATTR uint16_t s_accumulated_rain_min = 0;   // 過去24h積算雨量(分)
static RTC_DATA_ATTR uint8_t  s_last_water_day       = 0xFF; // 本日散水済みチェック(日: 1-31)
static RTC_DATA_ATTR uint32_t s_last_water_epoch     = 0;    // 前回散水時刻(Epoch)
static RTC_DATA_ATTR uint32_t s_last_nvs_backup_time = 0;   // 前回NVS保存時刻(Epoch)
static RTC_DATA_ATTR uint32_t s_cycle_count          = 0;   // 測定サイクル数

static ble_status_t s_ble_status = {0}; // BLE通知用ステータス

/* ハードウェア初期化 */
static void init_hardware(const lushgate_config_t *cfg)
{
    // ステータスLED (GPIO8)
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << PIN_STATUS_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL);

    // BOOTボタン (GPIO9 / 内蔵プルアップ / LOWアクティブ)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_USER_BUTTON),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // センサー＆ポンプドライバ初期化
    rain_sensor_init();
    pump_init(cfg->pump_active_level);
}

/* 起動時のBOOTボタン確認（1.5秒間のLED点滅中に押下でBLEモード） */
static bool check_boot_window_button(void)
{
    ESP_LOGI(TAG, "Checking BOOT window (1.5s)...");
    for (int i = 0; i < 15; i++) {
        gpio_set_level(PIN_STATUS_LED, (i % 2 == 0) ? LED_ACTIVE_LEVEL : (1 - LED_ACTIVE_LEVEL));
        if (gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
            ESP_LOGI(TAG, "🔘 BOOT button pressed during window! Entering BLE Mode.");
            gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL);
            vTaskDelay(pdMS_TO_TICKS(100)); // チャタリング除去
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL);
    return false;
}

/* BLE コールバック関数 */
static void on_ble_config_changed(const lushgate_config_t *new_cfg)
{
    ESP_LOGI(TAG, "BLE Config Updated: Sched=%02d:%02d, RainThresh=%dm, Pump=%ds",
             new_cfg->sched_hour, new_cfg->sched_min,
             new_cfg->rain_thresh_min, new_cfg->pump_total_sec);
    pump_set_active_level(new_cfg->pump_active_level);
}

static void on_ble_pump_cmd(uint8_t cmd, uint16_t duration_sec)
{
    if (cmd == 1) { // 散水ON
        uint16_t sec = duration_sec > 0 ? duration_sec : 180;
        ESP_LOGI(TAG, "Manual Watering Started via BLE (%d sec)", sec);
        pump_set_state(true);
        s_ble_status.pump_running = true;
        ble_gatt_notify_status();
    } else {        // 散水OFF
        ESP_LOGI(TAG, "Manual Watering Stopped via BLE");
        pump_set_state(false);
        s_ble_status.pump_running = false;
        ble_gatt_notify_status();
    }
}

static volatile bool s_ble_ever_connected = false;
static volatile bool s_ble_exit_requested = false;

static void on_ble_disconnect(void)
{
    ESP_LOGI(TAG, "BLE Disconnected callback triggered.");
    if (s_ble_ever_connected) {
        s_ble_exit_requested = true;
    }
}

/* BLEメンテナンスモード実行 */
static void run_ble_maintenance_mode(lushgate_config_t *cfg)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "📱 Entering BLE Maintenance Mode");
    ESP_LOGI(TAG, "   Device Name: LushGate");
    ESP_LOGI(TAG, "========================================");

    s_ble_ever_connected = false;
    s_ble_exit_requested = false;

    s_ble_status.rain_accum_min = s_accumulated_rain_min;
    s_ble_status.pump_running   = pump_is_running();
    s_ble_status.last_water_day = s_last_water_day;

    ble_gatt_set_config_changed_cb(on_ble_config_changed);
    ble_gatt_set_pump_cmd_cb(on_ble_pump_cmd);
    ble_gatt_set_disconnect_cb(on_ble_disconnect);

    if (ble_gatt_start(cfg, &s_ble_status) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE GATT server");
        return;
    }

    uint32_t max_duration_sec = cfg->ap_timeout_sec > 0 ? cfg->ap_timeout_sec : 300;
    uint32_t unconn_seconds = 0;
    uint32_t disc_countdown = 3;

    for (uint32_t i = 0; i < max_duration_sec; i++) {
        bool connected = ble_gatt_is_connected();
        if (connected) {
            s_ble_ever_connected = true;
            s_ble_exit_requested = false;
        } else {
            if (!s_ble_ever_connected) {
                unconn_seconds++;
                if (unconn_seconds >= 60) {
                    ESP_LOGI(TAG, "⏱️ No connection within 60s. Auto-sleeping to preserve battery...");
                    break;
                }
            } else if (s_ble_exit_requested) {
                disc_countdown--;
                if (disc_countdown == 0) {
                    ESP_LOGI(TAG, "👋 Maintenance completed and disconnected. Returning to Light Sleep.");
                    break;
                }
            }
        }

        // 省電力LEDインジケータ: 短パルス点滅 (点灯40ms / 消灯960ms: LED消費電力を70%削減)
        gpio_set_level(PIN_STATUS_LED, LED_ACTIVE_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(40));
        gpio_set_level(PIN_STATUS_LED, 1 - LED_ACTIVE_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(960));

        // 接続中のみ雨センサーを測定・Notify (未接続時は余分なADC動作を停止)
        if (connected) {
            rain_sensor_result_t live_rain = {0};
            rain_sensor_read(cfg->adc_thresh_mv, &live_rain);
            rain_sensor_power_down();

            s_ble_status.rain_accum_min = s_accumulated_rain_min;
            s_ble_status.pump_running   = pump_is_running();
            s_ble_status.last_water_day = s_last_water_day;
            s_ble_status.rain_raw_mv    = live_rain.voltage_mv;
            s_ble_status.is_raining     = live_rain.is_raining;
            ble_gatt_notify_status();
        }

        // BLEモード中であっても10秒ごとにスケジュール判定を実施
        if (i % 10 == 0) {
            run_normal_monitoring_cycle(cfg);
        }

        if (i % 30 == 0 && !connected) {
            ESP_LOGI(TAG, "BLE Advertising... (Waiting conn %lu/60s)", (unsigned long)unconn_seconds);
        }
    }

    ESP_LOGI(TAG, "Stopping BLE service & returning to low power sleep...");
    ble_gatt_stop();
    pump_set_state(false);
}

/* 通常監視サイクル (雨量測定 & 散水判定) */
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

    // 二重散水ガード:
    //  - 本日の日付 (tm_mday) で未判定であること
    //  - 前回散水時刻から最低12時間 (43200秒) 以上経過していること
    bool already_watered_today = (s_last_water_day == timeinfo.tm_mday);
    bool within_12h = (s_last_water_epoch > 0 && (now >= s_last_water_epoch) && ((now - s_last_water_epoch) < 43200));

    if (is_sched_time && !already_watered_today && !within_12h) {
        ESP_LOGI(TAG, "⏰ Schedule reached! (%02d:%02d) Checking rain accumulation...",
                 timeinfo.tm_hour, timeinfo.tm_min);

        // ★重要: 多重散水防止のため、実行判定直後に「本日散水済み」フラグとEpochを確定してNVSに即時保存★
        s_last_water_day = timeinfo.tm_mday;
        s_last_water_epoch = (uint32_t)now;
        uint16_t current_rain = s_accumulated_rain_min;
        s_accumulated_rain_min = 0; // 雨量カウンタを新サイクル用にリセット

        storage_save_last_state((uint32_t)now, s_accumulated_rain_min, s_last_water_day);
        s_last_nvs_backup_time = (uint32_t)now;

        water_history_entry_t entry = {
            .timestamp = (uint32_t)now,
            .rain_accum_min = current_rain,
            .battery_mv = 0,
        };

        if (current_rain >= cfg->rain_thresh_min) {
            ESP_LOGI(TAG, "💧 Sufficient rain (%d min >= %d min), SKIPPING watering today.",
                     current_rain, cfg->rain_thresh_min);
            entry.result = WATER_RESULT_SKIPPED_RAIN;
            entry.pump_run_sec = 0;
        } else {
            ESP_LOGI(TAG, "🌱 Insufficient rain (%d min < %d min), STARTING watering sequence (%d sec)!",
                     current_rain, cfg->rain_thresh_min, cfg->pump_total_sec);
            
            // ポンプデューティ散水実行
            pump_run_watering_cycle(cfg->pump_on_sec, cfg->pump_off_sec, cfg->pump_total_sec, NULL);
            entry.result = WATER_RESULT_WATERED;
            entry.pump_run_sec = cfg->pump_total_sec;
        }

        // 履歴をNVSに永続保存
        storage_add_history(&entry);

        // 散水完了後の最新時刻でNVS状態を再更新
        time_t finished_now = time(NULL);
        storage_save_last_state((uint32_t)finished_now, s_accumulated_rain_min, s_last_water_day);
        s_last_nvs_backup_time = (uint32_t)finished_now;
    } else if (is_sched_time) {
        ESP_LOGD(TAG, "⏰ Schedule window active, but already watered/skipped today (Day: %d, LastDay: %d, Within12h: %d)",
                 timeinfo.tm_mday, s_last_water_day, within_12h);
    }
}

/* メインエントリーポイント */
void app_main(void)
{
    esp_reset_reason_t rst_reason = esp_reset_reason();
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "    LushGate Starting Up (BLE Edition) ");
    ESP_LOGI(TAG, "    Reset Reason: %d", rst_reason);
    ESP_LOGI(TAG, "=======================================");

    // 0. タイムゾーン設定 (日本標準時: JST-9)
    setenv("TZ", "JST-9", 1);
    tzset();

    // 1. ストレージ初期化 & 設定読み出し
    storage_init();
    lushgate_config_t config;
    storage_load_config(&config);

    // 2. ハードウェア初期化
    init_hardware(&config);

    // 3. リセット時・電源投入時の時刻＆状態復旧
    bool time_is_valid = false;
    time_t now = time(NULL);
    if (now >= 1700000000) {
        time_is_valid = true;
    } else {
        uint32_t saved_time = 0;
        uint16_t saved_rain = 0;
        uint8_t saved_water_day = 0xFF;
        if (storage_load_last_state(&saved_time, &saved_rain, &saved_water_day) == ESP_OK && saved_time >= 1700000000) {
            struct timeval tv = { .tv_sec = (time_t)saved_time, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            if (s_accumulated_rain_min == 0) s_accumulated_rain_min = saved_rain;
            if (s_last_water_day == 0xFF)    s_last_water_day = saved_water_day;
            s_last_nvs_backup_time = saved_time;
            time_is_valid = true;
            ESP_LOGI(TAG, "✅ Restored RTC Time & Rain from NVS! (Epoch: %lu, Rain: %d min, LastDay: %d)",
                     (unsigned long)saved_time, s_accumulated_rain_min, s_last_water_day);
        } else {
            ESP_LOGW(TAG, "⚠️ RTC not synced yet. Starting BLE mode automatically for Web App setup...");
        }
    }

    // 4. 初回起動時の動作決定
    //    ・時刻未同期なら自動でBLEモード起動
    //    ・起動時にBOOTボタン押下があれば即座にBLEモード起動
    bool enter_ble = false;
    if (!time_is_valid) {
        enter_ble = true;
    } else if (gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
        enter_ble = true;
    } else {
        enter_ble = check_boot_window_button();
    }

    while (1) {
        if (enter_ble) {
            // BLEメンテナンスモード（設定・時刻同期・履歴確認・手動操作）
            run_ble_maintenance_mode(&config);
            enter_ble = false;

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

        ESP_LOGI(TAG, "Entering Light Sleep (%d sec)... [Press BOOT button anytime for BLE Mode]", config.sleep_interval_sec);
        fflush(stdout);
        esp_rom_delay_us(1000);

        // Light Sleep 突入 (RAM保持、RTC発振継続、消費電流 約0.13mA)
        esp_light_sleep_start();

        // --- 目覚めた後の処理 ---
        esp_sleep_wakeup_cause_t wake_reason = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Woke up from Light Sleep, reason: %d", wake_reason);

        // ボタン押下で起きた場合は次回ループでBLEモード起動
        if (wake_reason == ESP_SLEEP_WAKEUP_GPIO || gpio_get_level(PIN_USER_BUTTON) == BUTTON_PRESSED_LEVEL) {
            ESP_LOGI(TAG, "🔘 Woken up by BOOT button! Entering BLE Mode...");
            vTaskDelay(pdMS_TO_TICKS(50)); // チャタリング防止
            enter_ble = true;
        }
    }
}
