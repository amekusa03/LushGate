#include "storage_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "STORAGE";
#define NVS_NAMESPACE_CONFIG  "lush_cfg"
#define NVS_NAMESPACE_HISTORY "lush_hist"

#define KEY_CONFIG_BLOB       "cfg_blob"
#define KEY_HIST_BLOB         "hist_blob"
#define KEY_LAST_STATE        "last_state"

typedef struct {
    uint32_t timestamp;
    uint16_t rain_accum_min;
    uint8_t  last_water_day;
    uint8_t  reserved;
} __attribute__((packed)) last_state_t;

typedef struct {
    uint16_t head;
    uint16_t count;
    water_history_entry_t entries[MAX_HISTORY_ENTRIES];
} history_store_t;

static history_store_t s_history_store;
static bool s_history_loaded = false;

void storage_get_default_config(lushgate_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(lushgate_config_t));
    config->sleep_interval_sec = 180; // 3分
    config->sched_hour         = 7;   // 毎朝7時
    config->sched_min          = 0;
    config->rain_thresh_min    = 60;  // 60分
    config->adc_thresh_mv      = 60;
    config->pump_on_sec        = 180; // 3分ON
    config->pump_off_sec       = 120; // 2分OFF
    config->pump_total_sec     = 600; // 10分 (正味)
    config->ap_timeout_sec     = 300; // 5分
    config->pump_active_level  = 0;   // Active Low
    strncpy(config->ap_ssid, "LushGate", sizeof(config->ap_ssid) - 1);
    config->ap_pass[0] = '\0'; // デフォルトはオープン
}

esp_err_t storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // 履歴ストアの読み込み
    nvs_handle_t h;
    ret = nvs_open(NVS_NAMESPACE_HISTORY, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        size_t required_size = sizeof(history_store_t);
        ret = nvs_get_blob(h, KEY_HIST_BLOB, &s_history_store, &required_size);
        if (ret != ESP_OK || required_size != sizeof(history_store_t)) {
            ESP_LOGW(TAG, "History empty or size mismatch, initializing new history");
            memset(&s_history_store, 0, sizeof(history_store_t));
        }
        nvs_close(h);
    } else {
        memset(&s_history_store, 0, sizeof(history_store_t));
    }
    s_history_loaded = true;
    return ESP_OK;
}

esp_err_t storage_load_config(lushgate_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    storage_get_default_config(config);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config not found in NVS, using defaults");
        return ESP_OK;
    }

    size_t size = sizeof(lushgate_config_t);
    err = nvs_get_blob(h, KEY_CONFIG_BLOB, config, &size);
    nvs_close(h);

    if (err != ESP_OK || size != sizeof(lushgate_config_t)) {
        ESP_LOGW(TAG, "Config blob invalid, keeping defaults");
        storage_get_default_config(config);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Config loaded successfully. Schedule: %02d:%02d, RainThresh: %dm, PumpTotal: %ds",
             config->sched_hour, config->sched_min, config->rain_thresh_min, config->pump_total_sec);
    return ESP_OK;
}

esp_err_t storage_save_config(const lushgate_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for config write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, KEY_CONFIG_BLOB, config, sizeof(lushgate_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS successfully");
    } else {
        ESP_LOGE(TAG, "Failed to save config to NVS: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_save_last_state(uint32_t timestamp, uint16_t rain_accum_min, uint8_t last_water_day)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    last_state_t st = {
        .timestamp = timestamp,
        .rain_accum_min = rain_accum_min,
        .last_water_day = last_water_day,
        .reserved = 0
    };

    err = nvs_set_blob(h, KEY_LAST_STATE, &st, sizeof(last_state_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
        ESP_LOGD(TAG, "Saved last state to NVS: Epoch=%lu, Rain=%d min, LastWaterDay=%d",
                 (unsigned long)timestamp, rain_accum_min, last_water_day);
    }
    nvs_close(h);
    return err;
}

esp_err_t storage_load_last_state(uint32_t *timestamp, uint16_t *rain_accum_min, uint8_t *last_water_day)
{
    if (!timestamp || !rain_accum_min || !last_water_day) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    last_state_t st = {0};
    size_t size = sizeof(last_state_t);
    err = nvs_get_blob(h, KEY_LAST_STATE, &st, &size);
    nvs_close(h);

    if (err == ESP_OK && size == sizeof(last_state_t)) {
        *timestamp = st.timestamp;
        *rain_accum_min = st.rain_accum_min;
        *last_water_day = st.last_water_day;
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t save_history_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_HISTORY, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, KEY_HIST_BLOB, &s_history_store, sizeof(history_store_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t storage_add_history(const water_history_entry_t *entry)
{
    if (!entry) return ESP_ERR_INVALID_ARG;
    if (!s_history_loaded) storage_init();

    s_history_store.entries[s_history_store.head] = *entry;
    s_history_store.head = (s_history_store.head + 1) % MAX_HISTORY_ENTRIES;
    if (s_history_store.count < MAX_HISTORY_ENTRIES) {
        s_history_store.count++;
    }

    ESP_LOGI(TAG, "Added history entry. Total count: %d, Result: %d, RainAccum: %dm, PumpRun: %ds",
             s_history_store.count, entry->result, entry->rain_accum_min, entry->pump_run_sec);

    return save_history_to_nvs();
}

uint16_t storage_get_history_count(void)
{
    return s_history_store.count;
}

// 0 is newest, count-1 is oldest
esp_err_t storage_get_history_entry(uint16_t index, water_history_entry_t *entry)
{
    if (!entry || index >= s_history_store.count) return ESP_ERR_INVALID_ARG;

    // newest is at (head - 1 - index + MAX_HISTORY_ENTRIES) % MAX_HISTORY_ENTRIES
    int pos = (int)s_history_store.head - 1 - (int)index;
    while (pos < 0) pos += MAX_HISTORY_ENTRIES;
    pos = pos % MAX_HISTORY_ENTRIES;

    *entry = s_history_store.entries[pos];
    return ESP_OK;
}

esp_err_t storage_clear_history(void)
{
    memset(&s_history_store, 0, sizeof(history_store_t));
    return save_history_to_nvs();
}
