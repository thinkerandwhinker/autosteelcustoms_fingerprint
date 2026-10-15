#include "mode_manager.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_button.h"
#include "dns_hijack.h"
#include "nvs_config.h"
#include "ui_task.h"
#include "web_auth.h"
#include "web_server.h"
#include "wifi_ap.h"
#include "wifi_sta.h"

static const char *TAG = "mode_mgr";

#define RESET_FEEDBACK_DELAY_MS 1500

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_switching = false;

// only one mode runs per boot, so single initialization is enough
static void network_stack_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

void start_config_mode(void)
{
    ESP_LOGI(TAG, "Memulai MODE KONFIGURASI");
    network_stack_init();

    web_auth_init();

    esp_ip4_addr_t ap_ip;
    ESP_ERROR_CHECK(wifi_ap_start(&ap_ip));
    ESP_ERROR_CHECK(web_server_start(&ap_ip));
    ESP_ERROR_CHECK(dns_hijack_start(&ap_ip));

    esp_err_t err = config_button_start(CONFIG_BUTTON_MODE_RESET_LOGIN);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Tombol reset login tidak aktif: %s", esp_err_to_name(err));
    }
}

void start_operational_mode(void)
{
    ESP_LOGI(TAG, "Memulai MODE OPERASIONAL");
    network_stack_init();

    esp_err_t err = wifi_sta_start();
    if (err == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "SSID tidak tersedia saat memulai STA");
        // when SSID is not validate, ESP entering config mode
        mode_manager_enter_config("SSID kosong saat start STA", false);
        return;
    }
    ESP_ERROR_CHECK(err);

    err = config_button_start(CONFIG_BUTTON_MODE_ENTER_CONFIG);
    if (err != ESP_OK)
    {
        // this is not fatal, just means losing access to manual recovery, jadi gass ajalah
        ESP_LOGW(TAG, "Tombol masuk-config tidak aktif: %s", esp_err_to_name(err));
    }

    /**
     * TODO
     * next: mqtt_task, fpm_task
     *
     * bismillah, yang penting ini jalan dulu
     */
}

esp_err_t mode_manager_enter_config(const char *reason, bool user_initiated)
{
    portENTER_CRITICAL(&s_lock);
    bool already = s_switching;
    s_switching = true;
    portEXIT_CRITICAL(&s_lock);
    if (already)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Masuk mode konfigurasi pada boot berikutnya: %s", reason ? reason : "-");

    esp_err_t err = nvs_config_set_force_cfg(true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Gagal menyimpan penanda force_cfg: %s", esp_err_to_name(err));
        ui_feedback(UI_FEEDBACK_FAILURE, "Gagal", "Coba lagi");

        portENTER_CRITICAL(&s_lock);
        s_switching = false;
        portEXIT_CRITICAL(&s_lock);
        return err;
    }

    ui_feedback(user_initiated ? UI_FEEDBACK_SUCCESS : UI_FEEDBACK_FAILURE,
                "Mode Konfigurasi", "Restart...");
    vTaskDelay(pdMS_TO_TICKS(RESET_FEEDBACK_DELAY_MS)); // delay to give led and buzzer chance

    ESP_LOGW(TAG, "Restart");
    esp_restart();
}