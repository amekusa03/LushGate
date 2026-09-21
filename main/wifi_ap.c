#include "wifi_ap.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "mdns.h"
#include <string.h>

static const char *TAG = "WIFI_AP";
static bool s_wifi_active = false;
static esp_netif_t *s_ap_netif = NULL;

static void start_mdns_service(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    // ホスト名を lushgate に設定 -> http://lushgate.local でアクセス可能
    ESP_ERROR_CHECK(mdns_hostname_set("lushgate"));
    ESP_ERROR_CHECK(mdns_instance_name_set("LushGate Solar Watering System"));

    // HTTP サービス登録
    ESP_ERROR_CHECK(mdns_service_add("LushGate-Web", "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS responder started: http://lushgate.local");
}

esp_err_t wifi_ap_start(const char *ssid_override, const char *password)
{
    if (s_wifi_active) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    // SSIDの決定 (指定がなければ LushGate-XXXX)
    if (ssid_override && strlen(ssid_override) > 0) {
        strncpy((char *)wifi_config.ap.ssid, ssid_override, sizeof(wifi_config.ap.ssid) - 1);
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "LushGate-%02X%02X", mac[4], mac[5]);
    }
    wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);

    // パスワード設定 (8文字以上ならWPA2_PSK、それ以外はOPEN)
    if (password && strlen(password) >= 8) {
        strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_active = true;
    ESP_LOGI(TAG, "Wi-Fi AP started. SSID: [%s] Auth: %d", wifi_config.ap.ssid, wifi_config.ap.authmode);

    start_mdns_service();

    return ESP_OK;
}

void wifi_ap_stop(void)
{
    if (!s_wifi_active) return;

    mdns_free();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_wifi_active = false;
    ESP_LOGI(TAG, "Wi-Fi AP and mDNS stopped");
}

bool wifi_ap_is_active(void)
{
    return s_wifi_active;
}
