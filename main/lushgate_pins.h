#pragma once

#include "driver/gpio.h"

// ESP32-C3 Pin Assignments
#define PIN_RAIN_SENSE_A    GPIO_NUM_0    // ADC1_CH0 / InOut (極性反転・雨センサー電極A)
#define PIN_RAIN_SENSE_B    GPIO_NUM_1    // ADC1_CH1 / InOut (極性反転・雨センサー電極B)
#define PIN_PUMP_CTRL       GPIO_NUM_7    // Digital Output (フォトカプラ絶縁モジュール / リレー制御)
#define PIN_STATUS_LED      GPIO_NUM_8    // Digital Output (オンボードLED / 状態表示)
#define PIN_USER_BUTTON     GPIO_NUM_9    // Digital Input (BOOTボタン共用 / Pull-up / APモード起動)

// Active Levels
#define PUMP_ACTIVE_LEVEL   1             // High = リレーON
#define LED_ACTIVE_LEVEL    0             // Low = LED点灯 (ESP32-C3一般的なActive Low)
#define BUTTON_PRESSED_LEVEL 0            // Low = 押下 (Active Low)

// ADC Channels for Rain Sense
#define RAIN_ADC_UNIT       ADC_UNIT_1
#define RAIN_ADC_CH_A       ADC_CHANNEL_0 // GPIO0
#define RAIN_ADC_CH_B       ADC_CHANNEL_1 // GPIO1
