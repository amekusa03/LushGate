#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MAX_HISTORY_ENTRIES 60

typedef enum {
    WATER_RESULT_WATERED = 0,    // 散水実行完了
    WATER_RESULT_SKIPPED_RAIN,   // 降雨十分のためスキップ
    WATER_RESULT_MANUAL,         // 手動散水実行
    WATER_RESULT_ERROR           // エラー・異常停止
} water_result_t;

typedef struct {
    uint32_t timestamp;        // UNIX Epoch (秒)
    uint16_t rain_accum_min;   // 過去24h累積降雨時間 (分)
    uint16_t pump_run_sec;     // 実際のポンプ稼働時間 (秒)
    uint8_t  result;           // water_result_t
    uint16_t battery_mv;       // バッテリー電圧 (mV, 未計測時は0)
} __attribute__((packed)) water_history_entry_t;

typedef struct {
    uint16_t sleep_interval_sec; // 通常雨監視スリープ周期 (秒, デフォルト180)
    uint8_t  sched_hour;         // 散水判定時刻 時 (0-23, デフォルト7)
    uint8_t  sched_min;          // 散水判定時刻 分 (0-59, デフォルト0)
    uint16_t rain_thresh_min;    // 散水スキップ降雨閾値 (分, デフォルト60)
    uint16_t adc_thresh_mv;      // 雨滴検知ADC電圧閾値 (mV, デフォルト60)
    uint16_t pump_on_sec;        // ポンプONデューティ (秒, デフォルト180)
    uint16_t pump_off_sec;       // ポンプOFFデューティ (秒, デフォルト120)
    uint16_t pump_total_sec;     // ポンプ総散水時間 (秒, デフォルト600)
    uint16_t ap_timeout_sec;     // APモード自動停止時間 (秒, デフォルト300)
    uint8_t  pump_active_level;  // リレー駆動論理 (0: Active Low, 1: Active High, デフォルト0)
    char     ap_ssid[32];        // AP SSID
    char     ap_pass[64];        // AP パスワード (空ならオープン)
} lushgate_config_t;

esp_err_t storage_init(void);
void storage_get_default_config(lushgate_config_t *config);
esp_err_t storage_load_config(lushgate_config_t *config);
esp_err_t storage_save_config(const lushgate_config_t *config);

// 直近の状態（時刻、累積雨量、当日散水完了日）のバックアップと復元
esp_err_t storage_save_last_state(uint32_t timestamp, uint16_t rain_accum_min, uint8_t last_water_day);
esp_err_t storage_load_last_state(uint32_t *timestamp, uint16_t *rain_accum_min, uint8_t *last_water_day);

esp_err_t storage_add_history(const water_history_entry_t *entry);
uint16_t  storage_get_history_count(void);
esp_err_t storage_get_history_entry(uint16_t index, water_history_entry_t *entry);
esp_err_t storage_clear_history(void);
