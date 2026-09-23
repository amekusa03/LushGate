#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t pump_init(uint8_t active_level);
void pump_set_active_level(uint8_t active_level);
void pump_set_state(bool on);
bool pump_is_running(void);

// 自動散水デューティサイクル実行（ブロッキング/タスク内実行用）
// on_sec: ON時間, off_sec: OFF時間, total_on_sec: 正味総散水時間
// 実行中に *abort_requested が true になると中断
esp_err_t pump_run_watering_cycle(uint16_t on_sec, uint16_t off_sec, uint16_t total_on_sec, volatile bool *abort_requested);

// Web/手動テスト用: 指定秒数後に自動停止する手動ON
esp_err_t pump_manual_start(uint16_t duration_sec);
esp_err_t pump_manual_stop(void);
