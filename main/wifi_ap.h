#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_ap_start(const char *ssid, const char *password);
void wifi_ap_stop(void);
bool wifi_ap_is_active(void);
