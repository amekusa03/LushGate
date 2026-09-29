#include "pump_control.h"
/**
 * @file ble_gatt.c
 * @brief LushGate BLE GATTサービス実装 (NimBLE) - Android / iOS / Web Bluetooth 完全対応
 */

#include "ble_gatt.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

static const char *TAG = "BLE_GATT";

/* ===== UUIDs (Static Structs) ===== */
/* Service: 12340000-5678-1234-5678-000000000000 */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x56,
                     0x34,0x12,0x78,0x56,0x00,0x00,0x34,0x12);

/* CONFIG   char: 12340001-... */
static const ble_uuid128_t s_chr_config_uuid =
    BLE_UUID128_INIT(0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x56,
                     0x34,0x12,0x78,0x56,0x01,0x00,0x34,0x12);

/* TIMESYNC char: 12340002-... */
static const ble_uuid128_t s_chr_timesync_uuid =
    BLE_UUID128_INIT(0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x56,
                     0x34,0x12,0x78,0x56,0x02,0x00,0x34,0x12);

/* PUMP_CMD char: 12340003-... */
static const ble_uuid128_t s_chr_pump_cmd_uuid =
    BLE_UUID128_INIT(0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x56,
                     0x34,0x12,0x78,0x56,0x03,0x00,0x34,0x12);

/* STATUS   char: 12340004-... */
static const ble_uuid128_t s_chr_status_uuid =
    BLE_UUID128_INIT(0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x56,
                     0x34,0x12,0x78,0x56,0x04,0x00,0x34,0x12);

/* HISTORY  char: 12340005-... */
static const ble_uuid128_t s_chr_history_uuid =
    BLE_UUID128_INIT(0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x56,
                     0x34,0x12,0x78,0x56,0x05,0x00,0x34,0x12);

/* ===== 内部状態 ===== */
static lushgate_config_t *s_cfg = NULL;
static ble_status_t      *s_status = NULL;
static uint16_t           s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t           s_status_val_handle = 0;
static bool               s_status_notify_enabled = false;
static uint8_t            s_history_read_index = 0;
static uint8_t            s_own_addr_type;

static void (*s_cfg_changed_cb)(const lushgate_config_t *new_cfg) = NULL;
static void (*s_pump_cmd_cb)(uint8_t cmd, uint16_t duration_sec) = NULL;
static void (*s_disconnect_cb)(void) = NULL;

static void ble_start_advertising(void);

/* ===== ヘルパー: STATUS JSON 生成 ===== */
static void build_status_json(char *buf, size_t max_len)
{
    time_t now = time(NULL);
    bool time_synced = (now >= 1700000000);
    int rain = s_status ? s_status->rain_accum_min : 0;
    int pump = pump_is_running() ? 1 : 0;
    int day  = s_status ? s_status->last_water_day : 255;
    int hist = storage_get_history_count();
    int mv   = s_status ? s_status->rain_raw_mv : 0;
    int wet  = s_status ? (s_status->is_raining ? 1 : 0) : 0;

    snprintf(buf, max_len,
        "{\"rain\":%d,\"pump\":%d,\"synced\":%d,\"epoch\":%lu,\"day\":%d,\"hist_cnt\":%d,\"mv\":%d,\"wet\":%d}",
        rain, pump, time_synced ? 1 : 0, (unsigned long)now, day, hist, mv, wet);
}

/* ===== 各 characteristic のコールバック ===== */

/* 1. CONFIG (R/W) */
static int gatt_config_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!s_cfg) return BLE_ATT_ERR_UNLIKELY;
    int rc = os_mbuf_append(ctxt->om, s_cfg, sizeof(lushgate_config_t));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_config_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!s_cfg) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 17) {
        ESP_LOGW(TAG, "CONFIG write invalid length: %d (expected at least 17)", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    lushgate_config_t new_cfg = *s_cfg;
    size_t copy_len = len > sizeof(lushgate_config_t) ? sizeof(lushgate_config_t) : len;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &new_cfg, copy_len, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    *s_cfg = new_cfg;
    storage_save_config(s_cfg);
    ESP_LOGI(TAG, "Config updated & saved to NVS via BLE (len=%d)", len);

    if (s_cfg_changed_cb) s_cfg_changed_cb(s_cfg);
    return 0;
}

/* 2. TIMESYNC (W) */
static int gatt_timesync_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint32_t epoch = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &epoch, 4, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    setenv("TZ", "JST-9", 1);
    tzset();
    ESP_LOGI(TAG, "RTC time synchronized via BLE: %lu (JST set)", (unsigned long)epoch);

    ble_gatt_notify_status();
    return 0;
}

/* 3. PUMP_CMD (W) */
static int gatt_pump_cmd_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[3] = {0};
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len > 3 ? 3 : len, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    uint8_t  cmd          = buf[0];
    uint16_t duration_sec = 0;
    if (len >= 3) {
        duration_sec = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
    }

    ESP_LOGI(TAG, "Pump CMD received: %d, duration: %d sec", cmd, duration_sec);
    if (s_pump_cmd_cb) s_pump_cmd_cb(cmd, duration_sec);
    return 0;
}

/* 4. STATUS (R/Notify) */
static int gatt_status_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char json[256];
    build_status_json(json, sizeof(json));
    int rc = os_mbuf_append(ctxt->om, json, strlen(json));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* 5. HISTORY (R/W) */
static int gatt_history_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t idx = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &idx, 1, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    s_history_read_index = idx;
    return 0;
}

static int gatt_history_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    water_history_entry_t entry;
    esp_err_t err = storage_get_history_entry(s_history_read_index, &entry);

    char json[128];
    if (err != ESP_OK) {
        snprintf(json, sizeof(json), "{\"valid\":0}");
    } else {
        snprintf(json, sizeof(json),
            "{\"valid\":1,\"idx\":%d,\"ts\":%lu,\"rain\":%d,\"res\":%d,\"sec\":%d,\"mv\":%d}",
            s_history_read_index,
            (unsigned long)entry.timestamp,
            entry.rain_accum_min,
            entry.result,
            entry.pump_run_sec,
            entry.battery_mv);
    }

    int rc = os_mbuf_append(ctxt->om, json, strlen(json));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* R/W ディスパッチャラッパー */
static int gatt_config_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return gatt_config_read_cb(conn_handle, attr_handle, ctxt, arg);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        return gatt_config_write_cb(conn_handle, attr_handle, ctxt, arg);
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_timesync_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        return gatt_timesync_write_cb(conn_handle, attr_handle, ctxt, arg);
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_pump_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        return gatt_pump_cmd_write_cb(conn_handle, attr_handle, ctxt, arg);
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return gatt_status_read_cb(conn_handle, attr_handle, ctxt, arg);
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_history_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return gatt_history_read_cb(conn_handle, attr_handle, ctxt, arg);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        return gatt_history_write_cb(conn_handle, attr_handle, ctxt, arg);
    return BLE_ATT_ERR_UNLIKELY;
}

/* ===== GATTサービステーブル ===== */
static const struct ble_gatt_svc_def s_lushgate_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &s_chr_config_uuid.u,   .access_cb = gatt_config_access_cb,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_chr_timesync_uuid.u, .access_cb = gatt_timesync_access_cb,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_chr_pump_cmd_uuid.u, .access_cb = gatt_pump_cmd_access_cb,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_chr_status_uuid.u,   .access_cb = gatt_status_access_cb,
              .val_handle = &s_status_val_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &s_chr_history_uuid.u,  .access_cb = gatt_history_access_cb,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { 0 },
        },
    },
    { 0 },
};

/* ===== GAP イベントハンドラ ===== */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_conn_handle = event->connect.status == 0
                      ? event->connect.conn_handle
                      : BLE_HS_CONN_HANDLE_NONE;
        s_status_notify_enabled = false;
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE device connected (handle=%u)", s_conn_handle);
        } else {
            ESP_LOGI(TAG, "BLE connect failed, status=%d. Re-advertising.", event->connect.status);
            ble_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d).", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_status_notify_enabled = false;
        if (s_disconnect_cb) {
            s_disconnect_cb();
        }
        ble_start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        s_status_notify_enabled = (event->subscribe.cur_notify != 0);
        ESP_LOGI(TAG, "STATUS notify subscribe: %d (handle=%u)",
                 event->subscribe.cur_notify, event->subscribe.attr_handle);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: conn_handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection update: status=%d", event->conn_update.status);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        ESP_LOGI(TAG, "Connection update request received");
        return 0; // 提案された接続パラメータを承認

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        ESP_LOGI(TAG, "BLE Passkey action: %d", event->passkey.params.action);
        struct ble_sm_io pkey = {0};
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ESP_LOGI(TAG, "BLE Repeat pairing requested, deleting old keys & allowing retry");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption change: status=%d", event->enc_change.status);
        break;

    default:
        break;
    }
    return 0;
}

/* ===== アドバタイズ開始関数 ===== */
static void ble_start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = 0x0500, /* 800ms (大幅省電力) */
        .itvl_max  = 0x0640, /* 1000ms */
    };

    struct ble_hs_adv_fields adv_fields = {
        .flags           = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name            = (uint8_t *)"LushGate",
        .name_len        = 8,
        .name_is_complete = 1,
    };
    ble_gap_adv_set_fields(&adv_fields);

    /* スキャン応答に 128-bit サービスUUID を付与 */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = &s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp_fields);

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, ble_gap_event_cb, NULL);
    ESP_LOGI(TAG, "BLE advertising started (rc=%d)", rc);
}

/* ===== ホスト同期コールバック ===== */
static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    assert(rc == 0);

    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

/* ===== NimBLE ホストタスク ===== */
static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ===== 公開API ===== */

esp_err_t ble_gatt_start(lushgate_config_t *cfg, ble_status_t *status)
{
    s_cfg    = cfg;
    s_status = status;

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return ESP_FAIL;
    }

    /* NimBLE ホスト設定 */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* セキュリティ設定: パスキー不要 (Just Works) */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("LushGate");

    /* ストレージ設定初期化 (再ペアリング・鍵管理対応) */
    ble_store_config_init();

    rc = ble_gatts_count_cfg(s_lushgate_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_lushgate_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE GATT service started (Just Works mode, 5 characteristics)");
    return ESP_OK;
}

void ble_gatt_stop(void)
{
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status_notify_enabled = false;
    ESP_LOGI(TAG, "BLE GATT service stopped & deinitialized");
}

void ble_gatt_notify_status(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!s_status_notify_enabled) return;
    if (s_status_val_handle == 0) return;

    char json[256];
    build_status_json(json, sizeof(json));

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    }
}

void ble_gatt_set_config_changed_cb(void (*cb)(const lushgate_config_t *new_cfg))
{
    s_cfg_changed_cb = cb;
}

void ble_gatt_set_pump_cmd_cb(void (*cb)(uint8_t cmd, uint16_t duration_sec))
{
    s_pump_cmd_cb = cb;
}

bool ble_gatt_is_connected(void)
{
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

void ble_gatt_set_disconnect_cb(void (*cb)(void))
{
    s_disconnect_cb = cb;
}
