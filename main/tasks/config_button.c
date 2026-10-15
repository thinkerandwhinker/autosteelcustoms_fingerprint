#include "config_button.h"

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "mode_manager.h"
#include "nvs_config.h"
#include "ui_task.h"
#include "web_auth.h"

static const char *TAG = "cfg_button";

#define BUTTON_TASK_STACK 4096
#define BUTTON_TASK_PRIORITY 2
#define DEBOUNCE_AFTER_RELEASE_MS 100

static TaskHandle_t s_task = NULL;

static inline bool pressed(void)
{
    return gpio_get_level(PIN_BTN_CONFIG) == 0; // LOW active
}

static void wait_release(void)
{
    while (pressed())
    {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BUTTON_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_AFTER_RELEASE_MS));
}

static void button_task(void *arg)
{
    const config_button_mode_t mode = (config_button_mode_t)(intptr_t)arg;
    const uint32_t hold_ms = (mode == CONFIG_BUTTON_MODE_ENTER_CONFIG)
                                 ? CONFIG_BUTTON_ENTER_CONFIG_HOLD_MS
                                 : CONFIG_BUTTON_RESET_LOGIN_HOLD_MS;
    uint32_t held = 0;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BUTTON_POLL_MS));

        if (!pressed())
        {
            held = 0;
            continue;
        }
        held += CONFIG_BUTTON_POLL_MS;
        if (held < hold_ms)
        {
            continue;
        }

        if (mode == CONFIG_BUTTON_MODE_ENTER_CONFIG)
        {
            ESP_LOGW(TAG, "Tombol ditahan %u ms. Lepas tombol untuk masuk mode konfigurasi.", (unsigned)held);
            ui_feedback(UI_FEEDBACK_SUCCESS, "Lepas tombol", "untuk mode config");

            /**
             * must wait boot button until selesai dipencet
             * if GPIO0 is still LOW when the chip resets ESP will enter download mode
             * kalo udah begitu takutnya ESP dikira macet
             */
            wait_release();

            /**
             * doesn't return if successful, ESP will restart
             */
            mode_manager_enter_config("tombol BOOT ditahan", true);
        }
        else
        {
            ESP_LOGW(TAG, "Tombol ditahan %u ms. Mereset login ke default.", (unsigned)held);
            esp_err_t err = nvs_config_reset_auth();
            if (err == ESP_OK)
            {
                web_auth_invalidate_all();
                ui_feedback(UI_FEEDBACK_SUCCESS, "Login direset", "ke default");
            }
            else
            {
                ESP_LOGE(TAG, "Gagal mereset login: %s", esp_err_to_name(err));
                ui_feedback(UI_FEEDBACK_FAILURE, "Reset gagal", "Coba lagi");
            }
            wait_release();
        }
        held = 0; // achieved only if the action completes without a restart
    }
}

esp_err_t config_button_start(config_button_mode_t mode)
{
    if (s_task != NULL)
    {
        return ESP_OK;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN_CONFIG,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK)
    {
        return err;
    }

    if (xTaskCreate(button_task, "cfg_button", BUTTON_TASK_STACK, (void *)(intptr_t)mode,
                    BUTTON_TASK_PRIORITY, &s_task) != pdPASS)
    {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}