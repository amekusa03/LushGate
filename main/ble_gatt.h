/**
 * @file ble_gatt.h
 * @brief LushGate BLE GATTサービス (超低消費電力・NimBLE)
 *
 * Service UUID: 12340000-5678-1234-5678-000000000000
 *
 * Characteristics:
 *   CONFIG   (0001) R/W  - lushgate_config_t バイナリ
 *   TIMESYNC (0002) W    - UNIX Epoch uint32LE を書き込み → RTC更新
 *   PUMP_CMD (0003) W    - 0x00=OFF, 0x01=ON, [0x02, sec]=指定秒ON
 *   STATUS   (0004) R/N  - JSON文字列 (雨量・ポンプ状態・時刻・センサー生値)
 *   HISTORY  (0005) R/W  - Write: インデックス uint16LE / Read: 履歴エントリJSON
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "storage_manager.h"

/* 外部からアクセスできる状態 (main.cで更新) */
typedef struct {
    uint16_t rain_accum_min;
    uint8_t  last_water_day;
    uint32_t last_water_ts;
    bool     pump_running;
    uint16_t rain_raw_mv;    // リアルタイム雨センサー電圧 (mV)
    bool     is_raining;     // リアルタイム雨判定 (true=雨/濡れ, false=乾き)
} ble_status_t;

/**
 * @brief BLE GATTサービスを初期化して起動する
 * @param cfg  現在の設定 (GATTから読み書きする対象)
 * @param status 状態ポインタ (GATTから参照 / 外部から更新)
 */
esp_err_t ble_gatt_start(lushgate_config_t *cfg, ble_status_t *status);

/**
 * @brief BLE GATTサービスを停止する
 */
void ble_gatt_stop(void);

/**
 * @brief クライアントが接続中かどうか判定
 */
bool ble_gatt_is_connected(void);

/**
 * @brief STATUSキャラクタリスティックの変更を通知する (Notifyサポート)
 */
void ble_gatt_notify_status(void);

/**
 * @brief 設定変更コールバック登録
 */
void ble_gatt_set_config_changed_cb(void (*cb)(const lushgate_config_t *new_cfg));

/**
 * @brief ポンプコマンドコールバック登録
 */
void ble_gatt_set_pump_cmd_cb(void (*cb)(uint8_t cmd, uint16_t duration_sec));

/**
 * @brief クライアント切断時コールバック登録
 */
void ble_gatt_set_disconnect_cb(void (*cb)(void));
