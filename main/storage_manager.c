#include "storage_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "STORAGE";
#define NVS_NAMESPACE_CONFIG  "lush_cfg"
#define NVS_NAMESPACE_HISTORY "lush_hist"

#define KEY_CONFIG_BLOB       "cfg_blob"
#define KEY_ADC_MIG_120       "adc_mig120"
#define KEY_HIST_BLOB         "hist_blob"
#define KEY_LAST_STATE        "last_state"

typedef struct {
    uint32_t timestamp;
    uint16_t rain_accum_min;
    uint8_t  last_water_day;
    uint8_t  reserved;
    uint32_t last_water_epoch;
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
    config->adc_thresh_mv      = 120; // 120mV
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
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config not found in NVS, using defaults");
        return ESP_OK;
    }

    size_t size = sizeof(lushgate_config_t);
    err = nvs_get_blob(h, KEY_CONFIG_BLOB, config, &size);

    if (err != ESP_OK || size != sizeof(lushgate_config_t)) {
        nvs_close(h);
        ESP_LOGW(TAG, "Config blob invalid, keeping defaults");
        storage_get_default_config(config);
        return ESP_OK;
    }

    // 旧デフォルト閾値(60mV)が保存されている既存機を新デフォルト(120mV)へ初回自動移行
    uint8_t migrated = 0;
    if (nvs_get_u8(h, KEY_ADC_MIG_120, &migrated) != ESP_OK || migrated == 0) {
        if (config->adc_thresh_mv == 60) {
            config->adc_thresh_mv = 120;
            nvs_set_blob(h, KEY_CONFIG_BLOB, config, sizeof(lushgate_config_t));
            ESP_LOGI(TAG, "Migrated adc_thresh_mv from old default 60mV to 120mV");
        }
        nvs_set_u8(h, KEY_ADC_MIG_120, 1);
        nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded successfully. Schedule: %02d:%02d (SeasonalAuto:%d), RainThresh: %dm, AdcThresh: %dmV, PumpTotal: %ds",
             config->sched_hour, config->sched_min, config_is_seasonal_auto(config) ? 1 : 0,
             config->rain_thresh_min, config->adc_thresh_mv, config->pump_total_sec);
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

esp_err_t storage_save_last_state(uint32_t timestamp, uint16_t rain_accum_min, uint8_t last_water_day, uint32_t last_water_epoch)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    last_state_t st = {
        .timestamp = timestamp,
        .rain_accum_min = rain_accum_min,
        .last_water_day = last_water_day,
        .reserved = 0,
        .last_water_epoch = last_water_epoch
    };

    err = nvs_set_blob(h, KEY_LAST_STATE, &st, sizeof(last_state_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
        ESP_LOGD(TAG, "Saved last state to NVS: Epoch=%lu, Rain=%d min, LastWaterDay=%d, LastWaterEpoch=%lu",
                 (unsigned long)timestamp, rain_accum_min, last_water_day, (unsigned long)last_water_epoch);
    }
    nvs_close(h);
    return err;
}

esp_err_t storage_load_last_state(uint32_t *timestamp, uint16_t *rain_accum_min, uint8_t *last_water_day, uint32_t *last_water_epoch)
{
    if (!timestamp || !rain_accum_min || !last_water_day || !last_water_epoch) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    last_state_t st = {0};
    size_t size = sizeof(last_state_t);
    err = nvs_get_blob(h, KEY_LAST_STATE, &st, &size);
    nvs_close(h);

    if (err == ESP_OK) {
        *timestamp = st.timestamp;
        *rain_accum_min = st.rain_accum_min;
        *last_water_day = st.last_water_day;
        *last_water_epoch = (size >= sizeof(last_state_t)) ? st.last_water_epoch : 0;
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

esp_err_t storage_update_latest_history(const water_history_entry_t *entry)
{
    if (!entry) return ESP_ERR_INVALID_ARG;
    if (!s_history_loaded) storage_init();
    if (s_history_store.count == 0) {
        return storage_add_history(entry);
    }

    int pos = (int)s_history_store.head - 1;
    if (pos < 0) pos += MAX_HISTORY_ENTRIES;
    s_history_store.entries[pos] = *entry;

    ESP_LOGI(TAG, "Updated latest history entry. Result: %d, RainAccum: %dm, PumpRun: %ds",
             entry->result, entry->rain_accum_min, entry->pump_run_sec);

    return save_history_to_nvs();
}

esp_err_t storage_record_reset_event(uint32_t timestamp, uint16_t rain_accum_min, uint8_t result_type, uint16_t rst_reason)
{
    if (!s_history_loaded) storage_init();

    water_history_entry_t latest;
    if (storage_get_history_entry(0, &latest) == ESP_OK) {
        // 1. 直近の履歴が既にリセット系(BROWNOUT/RESET)なら、履歴枠を埋め尽くさないよう回数カウント(pump_run_sec)を+1して集約
        if (latest.result == WATER_RESULT_BROWNOUT || latest.result == WATER_RESULT_RESET) {
            if (timestamp > latest.timestamp) {
                latest.timestamp = timestamp;
            }
            latest.rain_accum_min = rain_accum_min;
            if (result_type == WATER_RESULT_BROWNOUT) {
                latest.result = WATER_RESULT_BROWNOUT; // 電圧低下を優先表示
            }
            if (latest.pump_run_sec < 65535) {
                latest.pump_run_sec += 1;
            }
            latest.battery_mv = rst_reason;
            ESP_LOGW(TAG, "Aggregated reset event (Result=%d, Count=%d, Reason=%d)",
                     latest.result, latest.pump_run_sec, rst_reason);
            return storage_update_latest_history(&latest);
        }
        // 2. 散水開始直後(300秒以内)にリレーON等でリセットし、直近が暫定ERROR記録だった場合はリセット原因で上書き更新
        if (latest.result == WATER_RESULT_ERROR && timestamp >= latest.timestamp && (timestamp - latest.timestamp) <= 300) {
            latest.timestamp = timestamp;
            latest.rain_accum_min = rain_accum_min;
            latest.result = result_type;
            latest.pump_run_sec = 1;
            latest.battery_mv = rst_reason;
            ESP_LOGW(TAG, "Upgraded pre-watering ERROR entry to reset event (Result=%d, Reason=%d)",
                     latest.result, rst_reason);
            return storage_update_latest_history(&latest);
        }
    }

    water_history_entry_t entry = {
        .timestamp      = timestamp,
        .rain_accum_min = rain_accum_min,
        .pump_run_sec   = 1, // 初回発生 (1回)
        .result         = result_type,
        .battery_mv     = rst_reason,
    };
    ESP_LOGW(TAG, "Recorded new reset event in history (Result=%d, Reason=%d)", result_type, rst_reason);
    return storage_add_history(&entry);
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
