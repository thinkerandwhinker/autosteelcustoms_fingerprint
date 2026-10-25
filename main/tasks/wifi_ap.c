#include "wifi_ap.h"

#include <stdio.h>

#include "esp_wifi.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"

#include "secret.h"

static const char *TAG = "wifi_ap";

#ifndef SECRET_AP_PASSWORD
#error "secret.h belum diperbarui. Salin makro terbaru (SECRET_AP_PASSWORD) dari secret_example.h"
#endif
/**
 * from now, AP using WPA2
 * user must using password to connect into ESP AP
 */
_Static_assert(sizeof(SECRET_AP_PASSWORD) - 1 >= 8 && sizeof(SECRET_AP_PASSWORD) - 1 <= 63,
               "SECRET_AP_PASSWORD harus 8-63 karakter");
#define AP_PASSWORD SECRET_AP_PASSWORD
#define AP_CHANNEL 1
#define AP_MAX_CONN 4

esp_err_t wifi_ap_start(esp_ip4_addr_t *ip_out)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL)
    {
        ESP_LOGE(TAG, "Gagal membuat netif AP");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    // wifi configuration using NVS as source of truth, it doesn't be persist by any drivers anymore
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));

    wifi_config_t wc = {0};
    int n = snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid),
                     "Presensi-Setup-%02X%02X", mac[4], mac[5]);
    wc.ap.ssid_len = (uint8_t)n;
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.pmf_cfg.required = false;

    strlcpy((char *)wc.ap.password, AP_PASSWORD, sizeof(wc.ap.password));
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &info));
    *ip_out = info.ip;

    ESP_LOGI(TAG, "AP aktif. SSID='%s' (WPA2) IP=" IPSTR, (char *)wc.ap.ssid, IP2STR(&info.ip));
    return ESP_OK;
}