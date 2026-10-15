#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "mode_manager.h"
#include "nvs_config.h"
#include "state_manager.h"
#include "ui_task.h"

static const char *TAG = "main";

void app_main(void)
{
    // init
    ESP_ERROR_CHECK(state_manager_nvs_init());
    ESP_ERROR_CHECK(state_manager_init());

    // mode deciding
    const bool complete = check_credentials_exist();
    const bool forced = nvs_config_is_force_cfg();
    const bool config_mode = forced || !complete;

    if (config_mode)
    {
        xEventGroupSetBits(sys_state_group, MODE_CONFIG_BIT);
        if (forced)
        {
            esp_err_t err = nvs_config_set_force_cfg(false);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Gagal menghapus force_cfg: %s", esp_err_to_name(err));
            }
        }
    }
    else
    {
        xEventGroupClearBits(sys_state_group, MODE_CONFIG_BIT);
    }

    // start ui task as feedback
    ESP_ERROR_CHECK(ui_task_start());

    // boot manager
    if (config_mode)
    {
        ESP_LOGW(TAG, "-> MODE KONFIGURASI (%s)", forced ? "dipaksa" : "konfigurasi belum lengkap");
        start_config_mode();
    }
    else
    {
        ESP_LOGI(TAG, "-> MODE OPERASIONAL");
        start_operational_mode();
    }
}