#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    uint16_t current_rain_accum_min;
    bool request_sleep;
} web_server_state_t;

extern web_server_state_t g_web_state;

esp_err_t web_server_start(void);
void web_server_stop(void);
