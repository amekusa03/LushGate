#pragma once

#include "driver/gpio.h"

// ESP32-C3 Pin Assignments
#define PIN_RAIN_OUT        GPIO_NUM_0    // ADC1_CH0 / Input (J3Yトランジスタ増幅出力 OUT)
#define PIN_RAIN_POWER      GPIO_NUM_1    // Digital Output (雨センサー給電パルス制御 VCC / 腐食・待機電力防止)
#define PIN_PUMP_CTRL       GPIO_NUM_7    // Digital Output (フォトカプラ絶縁モジュール / リレー制御)
#define PIN_STATUS_LED      GPIO_NUM_8    // Digital Output (オンボードLED / 状態表示)
#define PIN_USER_BUTTON     GPIO_NUM_9    // Digital Input (BOOTボタン共用 / Pull-up / APモード起動)

// Active Levels
#define PUMP_ACTIVE_LEVEL   1             // High = リレーON
#define LED_ACTIVE_LEVEL    0             // Low = LED点灯 (ESP32-C3一般的なActive Low)
#define BUTTON_PRESSED_LEVEL 0            // Low = 押下 (Active Low)

// ADC Channels for Rain Sense
#define RAIN_ADC_UNIT       ADC_UNIT_1
#define RAIN_ADC_CH         ADC_CHANNEL_0 // GPIO0 (OUT端子接続)

// 互換性エイリアス
#define PIN_RAIN_SENSE_A    PIN_RAIN_OUT
#define PIN_RAIN_SENSE_B    PIN_RAIN_POWER
#define RAIN_ADC_CH_A       RAIN_ADC_CH
#define RAIN_ADC_CH_B       RAIN_ADC_CH
