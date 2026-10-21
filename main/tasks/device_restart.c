#include "device_restart.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "ui_task.h"
#include "mqtt_service.h"
#include "command_guard.h"
#include "state_manager.h"

static const char *TAG = "dev_restart";

#define CHANNEL_RESTART "restart"
#define FLUSH_TIMEOUT_MS 3000
#define RESTART_UI_DELAY_MS 1200

static void publish_failed(const mqtt_command_t *cmd, const char *reason, const char *message)
{
    cJSON *o = mqtt_event_new("RESTART_FAILED", cmd->request_id);
    if (o == NULL)
        return;
    cJSON_AddStringToObject(o, "reason", reason);
    if (message)
        cJSON_AddStringToObject(o, "message", message);
    mqtt_service_publish_json(CHANNEL_RESTART, o, 1);
    cJSON_Delete(o);
}

/**
 * flow: lock => restart
 */
static void restart_handler(const mqtt_command_t *cmd, void *ctx)
{
    (void)ctx;

    // is same req or expired?
    if (command_guard_check(cmd) != GUARD_OK)
    {
        return;
    }

    // is being handle other command?
    if (xEventGroupGetBits(sys_state_group) & DEVICE_BUSY_BIT)
    {
        ESP_LOGW(TAG, "RESTART ditolak: perangkat sibuk");
        publish_failed(cmd, "BUSY", "perangkat sedang sibuk");
        return;
    }

    // lock
    xEventGroupSetBits(sys_state_group, DEVICE_BUSY_BIT);
    command_guard_commit(cmd, true);

    cJSON *ack = mqtt_event_new("RESTART_ACK", cmd->request_id);
    if (ack)
    {
        // confirm into backend
        mqtt_service_publish_json(CHANNEL_RESTART, ack, 1);
        cJSON_Delete(ack);
    }
    ui_feedback(UI_FEEDBACK_SUCCESS, "Restart", "Perintah server");

    cJSON *starting = mqtt_event_new("RESTART_STARTING", cmd->request_id);
    if (starting)
    {
        esp_err_t err = mqtt_service_publish_confirmed(CHANNEL_RESTART, starting, FLUSH_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "RESTART_STARTING belum terkonfirmasi (%s), tetap restart", esp_err_to_name(err));
        }
        cJSON_Delete(starting);
    }

    // stop mqtt gracefully
    esp_err_t mqtt_stop = mqtt_service_stop();
    if (mqtt_stop != ESP_OK)
    {
        // just show logs
        ESP_LOGW(TAG, "Gagal menutup koneksi MQTT: %s", esp_err_to_name(mqtt_stop));
    }

    ESP_LOGI(TAG, "Sisa stack minimum mqtt_task: %u byte", (unsigned)uxTaskGetStackHighWaterMark(NULL)); // set mqtt stack to null/clear
    vTaskDelay(pdMS_TO_TICKS(RESTART_UI_DELAY_MS));
    ESP_LOGW(TAG, "Restart atas perintah backend (requestId=%s)", cmd->request_id);

    // restart
    esp_restart();
}

// init
esp_err_t device_restart_init(void)
{
    return mqtt_service_register_command("RESTART", restart_handler, NULL);
}