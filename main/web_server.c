#include "web_server.h"
#include "storage_manager.h"
#include "rain_sensor.h"
#include "pump_control.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t s_server = NULL;
web_server_state_t g_web_state = {0};

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Helper to parse simple JSON int values
static int parse_json_int(const char *json, const char *key, int default_val)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    char *pos = strstr(json, pattern);
    if (!pos) return default_val;
    pos = strchr(pos, ':');
    if (!pos) return default_val;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return atoi(pos);
}

// GET /
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const size_t index_len = index_html_end - index_html_start;
    httpd_resp_send(req, (const char *)index_html_start, index_len);
    return ESP_OK;
}

// GET /api/status
static esp_err_t status_handler(httpd_req_t *req)
{
    lushgate_config_t cfg;
    storage_load_config(&cfg);

    time_t now = time(NULL);

    rain_sensor_result_t rain_res = {0};
    rain_sensor_read(cfg.adc_thresh_mv, &rain_res);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"time\":%ld,\"rain_accum_min\":%d,\"rain_thresh_min\":%d,"
        "\"sched_hour\":%d,\"sched_min\":%d,\"pump_total_sec\":%d,"
        "\"sensor_raw\":%d,\"sensor_mv\":%d,\"is_raining\":%s}",
        (long)now, g_web_state.current_rain_accum_min, cfg.rain_thresh_min,
        cfg.sched_hour, cfg.sched_min, cfg.pump_total_sec,
        rain_res.raw_adc, rain_res.voltage_mv, rain_res.is_raining ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/time
static esp_err_t time_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    long epoch = (long)parse_json_int(buf, "epoch", 0);
    if (epoch > 0) {
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "RTC Time synchronized to Epoch: %ld", epoch);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /api/config
static esp_err_t config_get_handler(httpd_req_t *req)
{
    lushgate_config_t cfg;
    storage_load_config(&cfg);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"sleep_interval_sec\":%d,\"sched_hour\":%d,\"sched_min\":%d,"
        "\"rain_thresh_min\":%d,\"adc_thresh_mv\":%d,\"pump_on_sec\":%d,"
        "\"pump_off_sec\":%d,\"pump_total_sec\":%d,\"ap_timeout_sec\":%d}",
        cfg.sleep_interval_sec, cfg.sched_hour, cfg.sched_min,
        cfg.rain_thresh_min, cfg.adc_thresh_mv, cfg.pump_on_sec,
        cfg.pump_off_sec, cfg.pump_total_sec, cfg.ap_timeout_sec);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/config
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    lushgate_config_t cfg;
    storage_load_config(&cfg);

    cfg.sched_hour         = parse_json_int(buf, "sched_hour", cfg.sched_hour);
    cfg.sched_min          = parse_json_int(buf, "sched_min", cfg.sched_min);
    cfg.sleep_interval_sec = parse_json_int(buf, "sleep_interval_sec", cfg.sleep_interval_sec);
    cfg.rain_thresh_min    = parse_json_int(buf, "rain_thresh_min", cfg.rain_thresh_min);
    cfg.adc_thresh_mv      = parse_json_int(buf, "adc_thresh_mv", cfg.adc_thresh_mv);
    cfg.pump_on_sec        = parse_json_int(buf, "pump_on_sec", cfg.pump_on_sec);
    cfg.pump_off_sec       = parse_json_int(buf, "pump_off_sec", cfg.pump_off_sec);
    cfg.pump_total_sec     = parse_json_int(buf, "pump_total_sec", cfg.pump_total_sec);
    cfg.ap_timeout_sec     = parse_json_int(buf, "ap_timeout_sec", cfg.ap_timeout_sec);

    storage_save_config(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /api/history
static esp_err_t history_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    uint16_t count = storage_get_history_count();
    char entry_buf[128];
    for (uint16_t i = 0; i < count; i++) {
        water_history_entry_t entry;
        if (storage_get_history_entry(i, &entry) == ESP_OK) {
            snprintf(entry_buf, sizeof(entry_buf),
                "%s{\"timestamp\":%ld,\"rain_accum_min\":%d,\"pump_run_sec\":%d,\"result\":%d,\"battery_mv\":%d}",
                (i > 0) ? "," : "", (long)entry.timestamp, entry.rain_accum_min,
                entry.pump_run_sec, entry.result, entry.battery_mv);
            httpd_resp_sendstr_chunk(req, entry_buf);
        }
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// GET /api/history/csv
static esp_err_t history_csv_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"lushgate_history.csv\"");

    httpd_resp_sendstr_chunk(req, "Timestamp,RainAccumMin,Result,PumpRunSec,BatteryMv\n");

    uint16_t count = storage_get_history_count();
    char line[128];
    for (uint16_t i = 0; i < count; i++) {
        water_history_entry_t entry;
        if (storage_get_history_entry(i, &entry) == ESP_OK) {
            snprintf(line, sizeof(line), "%ld,%d,%d,%d,%d\n",
                (long)entry.timestamp, entry.rain_accum_min, entry.result,
                entry.pump_run_sec, entry.battery_mv);
            httpd_resp_sendstr_chunk(req, line);
        }
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// POST /api/history/clear
static esp_err_t history_clear_handler(httpd_req_t *req)
{
    storage_clear_history();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/pump/test
static esp_err_t pump_test_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    if (strstr(buf, "\"action\":\"start\"")) {
        int duration = parse_json_int(buf, "duration_sec", 30);
        pump_manual_start(duration);
    } else {
        pump_manual_stop();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/system/sleep
static esp_err_t sleep_handler(httpd_req_t *req)
{
    g_web_state.request_sleep = true;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t uri_time = { .uri = "/api/time", .method = HTTP_POST, .handler = time_handler };
    httpd_uri_t uri_cfg_get = { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler };
    httpd_uri_t uri_cfg_post = { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler };
    httpd_uri_t uri_hist_get = { .uri = "/api/history", .method = HTTP_GET, .handler = history_get_handler };
    httpd_uri_t uri_hist_csv = { .uri = "/api/history/csv", .method = HTTP_GET, .handler = history_csv_handler };
    httpd_uri_t uri_hist_clr = { .uri = "/api/history/clear", .method = HTTP_POST, .handler = history_clear_handler };
    httpd_uri_t uri_pump_tst = { .uri = "/api/pump/test", .method = HTTP_POST, .handler = pump_test_handler };
    httpd_uri_t uri_sleep = { .uri = "/api/system/sleep", .method = HTTP_POST, .handler = sleep_handler };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_time);
    httpd_register_uri_handler(s_server, &uri_cfg_get);
    httpd_register_uri_handler(s_server, &uri_cfg_post);
    httpd_register_uri_handler(s_server, &uri_hist_get);
    httpd_register_uri_handler(s_server, &uri_hist_csv);
    httpd_register_uri_handler(s_server, &uri_hist_clr);
    httpd_register_uri_handler(s_server, &uri_pump_tst);
    httpd_register_uri_handler(s_server, &uri_sleep);

    ESP_LOGI(TAG, "Web Server started successfully on port 80");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web Server stopped");
    }
}
