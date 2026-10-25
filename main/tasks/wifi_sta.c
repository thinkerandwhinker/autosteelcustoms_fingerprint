#include "wifi_sta.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "mode_manager.h"
#include "nvs_config.h"
#include "state_manager.h"

static const char *TAG = "wifi_sta";

static int s_fail_count = 0;
static int s_auth_fail_count = 0;
static bool s_verified = false;
static esp_timer_handle_t s_retry_timer = NULL;

/**
 * is AP reject credential? what his reason?
 */
static bool is_auth_failure(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return true;
    default:
        return false;
    }
}

static uint32_t retry_delay_ms(int fail_count)
{
    int shift = fail_count - 1;
    if (shift < 0)
        shift = 0;
    if (shift > 4)
        shift = 4;
    uint32_t d = WIFI_RETRY_BASE_DELAY_MS << shift;
    return d > WIFI_RETRY_MAX_DELAY_MS ? WIFI_RETRY_MAX_DELAY_MS : d;
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
            const bool auth_fail = is_auth_failure(ev->reason);

            xEventGroupClearBits(sys_state_group, WIFI_CONNECTED_BIT);
            s_fail_count++;
            if (auth_fail)
                s_auth_fail_count++;

            ESP_LOGW(TAG, "Terputus/gagal (reason=%d%s) total=%d auth=%d verified=%d",
                     ev->reason, auth_fail ? ", auth" : "",
                     s_fail_count, s_auth_fail_count, (int)s_verified);

            const char *cfg_cause = NULL;

            // is retry attempt reached his limit?
            if (s_auth_fail_count >= WIFI_MAX_CONSECUTIVE_FAILURES)
            {
                cfg_cause = "autentikasi WiFi ditolak berulang (password salah/berubah)";
            }
            // is credential already wron at first time since configuration applied?
            else if (!s_verified && s_fail_count >= WIFI_MAX_CONSECUTIVE_FAILURES)
            {
                cfg_cause = "kredensial belum pernah berhasil dan gagal terus";
            }

            // is cause was not NULL?
            if (cfg_cause != NULL)
            {
                // does not return upon success (restart)
                // if it fails (NVS error), it proceeds with a standard retry
                mode_manager_enter_config(cfg_cause, false);
            }

            uint32_t delay = retry_delay_ms(s_fail_count);
            ESP_LOGI(TAG, "Mencoba lagi dalam %u ms", (unsigned)delay);
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000ULL);
            break;
        }
        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_fail_count = 0;
        s_auth_fail_count = 0;
        xEventGroupSetBits(sys_state_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Terhubung. IP=" IPSTR, IP2STR(&ev->ip_info.ip));

        if (!s_verified)
        {
            if (nvs_config_set_wifi_verified(true) == ESP_OK)
            {
                s_verified = true;
                ESP_LOGI(TAG, "Kredensial WiFi ditandai terverifikasi");
            }
        }
    }
}

esp_err_t wifi_sta_start(void)
{
    app_config_t *cfg = calloc(1, sizeof(app_config_t));
    if (cfg == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    nvs_config_load(cfg);

    if (cfg->wifi_ssid[0] == '\0')
    {
        free(cfg);
        return ESP_ERR_INVALID_STATE;
    }
    s_verified = nvs_config_is_wifi_verified();

    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (netif == NULL)
    {
        free(cfg);
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {0};
    // ensuring credentials are not truncated
    memcpy(wc.sta.ssid, cfg->wifi_ssid, strnlen(cfg->wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, cfg->wifi_pass, strnlen(cfg->wifi_pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = (cfg->wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG, "Menghubungkan ke SSID '%s' (%s, %s)", cfg->wifi_ssid,
             cfg->wifi_pass[0] ? "secured" : "open",
             s_verified ? "verified" : "belum verified");
    free(cfg);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}