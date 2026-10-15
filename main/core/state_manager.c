#include "state_manager.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "nvs_config.h"

static const char *TAG = "state_mgr";

EventGroupHandle_t sys_state_group = NULL;

esp_err_t state_manager_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Partisi NVS perlu di-erase (%s)", esp_err_to_name(ret));
        esp_err_t erase_ret = nvs_flash_erase();
        if (erase_ret != ESP_OK)
        {
            return erase_ret;
        }
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t state_manager_init(void)
{
    if (sys_state_group != NULL)
    {
        return ESP_OK;
    }

    sys_state_group = xEventGroupCreate();
    if (sys_state_group == NULL)
    {
        ESP_LOGE(TAG, "Gagal membuat event group");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool check_credentials_exist(void)
{
    uint32_t missing = nvs_config_missing_mask();
    if (missing != 0)
    {
        ESP_LOGI(TAG, "Konfigurasi belum lengkap (mask=0x%02X)", (unsigned)missing);
    }
    return missing == 0;
}