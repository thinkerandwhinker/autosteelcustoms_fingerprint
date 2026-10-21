#include "device_reset.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "as608.h"
#include "command_guard.h"
#include "mqtt_service.h"
#include "nvs_config.h"
#include "state_manager.h"
#include "ui_task.h"

static const char *TAG = "dev_reset";

#define CHANNEL_RESET "reset"
#define PUBLISH_TIMEOUT_MS 3000
#define PUBLISH_TRIES 3
#define PUBLISH_RETRY_DELAY_MS 300
#define RESTART_UI_DELAY_MS 1200

typedef struct
{
    uint16_t fingerprints_cleared;
    bool config_cleared;
} reset_actions_t;

/**
 * export interface EspResetReport {
 *      event: string,
 *      requestId: string,
 *      success: boolean,
 *      actions: {
 *          fingerprintsCleared: boolean,
 *          configCleared: boolean,
 *      },
 *      reason?: any,
 *      message?: string,
 * }
 */
static cJSON *build_report(const mqtt_command_t *cmd, bool success, const reset_actions_t *a, const char *reason, const char *message)
{
    cJSON *o = mqtt_event_new("RESET_REPORT", cmd->request_id);
    if (o == NULL)
        return NULL;

    cJSON *actions = cJSON_AddObjectToObject(o, "actions");
    if (cJSON_AddBoolToObject(o, "success", success) == NULL || actions == NULL ||
        cJSON_AddNumberToObject(actions, "fingerprintsCleared", a->fingerprints_cleared) == NULL ||
        cJSON_AddBoolToObject(actions, "configCleared", a->config_cleared) == NULL)
    {
        cJSON_Delete(o);
        return NULL;
    }
    if (reason)
        cJSON_AddStringToObject(o, "reason", reason);
    if (message)
        cJSON_AddStringToObject(o, "message", message);
    return o;
}

/**
 * what if, backend disconnected/not send response at the end of reset process?
 */
static esp_err_t publish_confirmed_retry(const cJSON *obj)
{
    esp_err_t err = ESP_FAIL;
    // repeat publis
    for (int i = 0; i < PUBLISH_TRIES; i++)
    {
        // wait until published message receives response from the backend
        err = mqtt_service_publish_confirmed(CHANNEL_RESET, obj, PUBLISH_TIMEOUT_MS);

        // got backend response?
        if (err == ESP_OK)
            return ESP_OK;
        ESP_LOGW(TAG, "Publish belum terkonfirmasi (%s), percobaan %d/%d", esp_err_to_name(err), i + 1, PUBLISH_TRIES);
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_RETRY_DELAY_MS));
    }
    return err;
}

/**
 * when reset failed
 */
static void fail_and_unlock(const mqtt_command_t *cmd, const reset_actions_t *a, const char *reason, const char *message)
{
    ESP_LOGE(TAG, "RESET_DEVICE gagal: %s (%s)", reason, message ? message : "-");

    cJSON *o = build_report(cmd, false, a, reason, message);
    if (o)
    {
        mqtt_service_publish_json(CHANNEL_RESET, o, 1);
        cJSON_Delete(o);
    }
    ui_feedback(UI_FEEDBACK_FAILURE, "Reset Gagal", reason);
    xEventGroupClearBits(sys_state_group, DEVICE_BUSY_BIT);
}

/**
 * wipe all fingerprint templat
 *
 * @return NULL or reason
 */
static const char *wipe_sensor(reset_actions_t *a, const char **message)
{
    // is sensor still there?
    if (!as608_is_ready())
    {
        *message = "sensor sidik jari tidak siap";
        return "SENSOR_ERROR";
    }

    uint16_t before = 0;
    as608_status_t st = as608_template_count(&before); // count temolate
    if (st != AS608_OK)
    {
        ESP_LOGE(TAG, "Baca jumlah template: %s", as608_status_str(st));
        *message = "gagal membaca jumlah template";
        return "SENSOR_ERROR";
    }

    st = as608_empty_database(); // clear template
    if (st != AS608_OK)
    {
        ESP_LOGE(TAG, "Kosongkan sensor: %s", as608_status_str(st));
        *message = "gagal mengosongkan sensor";
        return "SENSOR_ERROR";
    }

    uint16_t after = 0;
    st = as608_template_count(&after); // verify template count
    if (st != AS608_OK)
    {
        ESP_LOGE(TAG, "Verifikasi sensor: %s", as608_status_str(st));
        *message = "gagal memverifikasi sensor";
        return "SENSOR_ERROR";
    }

    a->fingerprints_cleared = (before > after) ? (uint16_t)(before - after) : 0;

    // is sensor already empty?
    if (after != 0)
    {
        *message = "sensor belum kosong setelah dikosongkan";
        return "SENSOR_ERROR";
    }
    return NULL;
}

/**
 * flow: guard => busy? => lock => sensor => NVS => report => farewell => restart
 */
static void reset_handler(const mqtt_command_t *cmd, void *ctx)
{
    (void)ctx;
    reset_actions_t actions = {0};

    // is same req or expired?
    if (command_guard_check(cmd) != GUARD_OK)
    {
        return;
    }

    // is ESP being bussy?
    if (xEventGroupGetBits(sys_state_group) & DEVICE_BUSY_BIT)
    {
        ESP_LOGW(TAG, "RESET_DEVICE ditolak: perangkat sibuk");
        cJSON *o = build_report(cmd, false, &actions, "BUSY", "perangkat sedang sibuk");
        if (o)
        {
            mqtt_service_publish_json(CHANNEL_RESET, o, 1);
            cJSON_Delete(o);
        }
        return;
    }

    // lock
    xEventGroupSetBits(sys_state_group, DEVICE_BUSY_BIT);
    command_guard_commit(cmd, true);
    ui_feedback(UI_FEEDBACK_SUCCESS, "Reset Perangkat", "Perintah server");

    /**
     * keep device_id in RAM before NVS is gone
     */
    char device_id[MAX_LEN_DEVICE_ID + 1];
    strlcpy(device_id, mqtt_service_device_id(), sizeof(device_id));
    ESP_LOGW(TAG, "RESET_DEVICE untuk %s (requestId=%s)", device_id, cmd->request_id);

    // sensor first, if this fails the config stays so backend can still reach this device
    const char *message = NULL;
    const char *reason = wipe_sensor(&actions, &message);

    // is sensor wiping not success?
    if (reason != NULL)
    {
        fail_and_unlock(cmd, &actions, reason, message);
        return;
    }

    // NVS last, erased + read back
    esp_err_t err = nvs_config_erase_all();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Hapus NVS: %s", esp_err_to_name(err));
        fail_and_unlock(cmd, &actions, "STORAGE_ERROR", "gagal menghapus konfigurasi");
        return;
    }
    actions.config_cleared = true;

    // from here there is no way back
    cJSON *report = build_report(cmd, true, &actions, NULL, NULL);
    err = (report != NULL) ? publish_confirmed_retry(report) : ESP_ERR_NO_MEM;
    cJSON_Delete(report);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RESET_REPORT tidak terkonfirmasi (%s), konfigurasi sudah terhapus", esp_err_to_name(err));
    }

    cJSON *farewell = mqtt_event_new("FAREWELL", cmd->request_id);
    err = (farewell != NULL) ? publish_confirmed_retry(farewell) : ESP_ERR_NO_MEM;
    cJSON_Delete(farewell);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "FAREWELL tidak terkonfirmasi (%s), tetap restart", esp_err_to_name(err));
    }

    ui_feedback(UI_FEEDBACK_SUCCESS, "Reset Selesai", "Restart...");
    esp_err_t stop = mqtt_service_stop();
    if (stop != ESP_OK)
    {
        // no problem, gas aja, just show the logs
        ESP_LOGW(TAG, "Gagal menutup koneksi MQTT: %s", esp_err_to_name(stop));
    }

    ESP_LOGI(TAG, "Sisa stack minimum mqtt_task: %u byte", (unsigned)uxTaskGetStackHighWaterMark(NULL)); // set mqtt stack tp null/clear
    vTaskDelay(pdMS_TO_TICKS(RESTART_UI_DELAY_MS));
    ESP_LOGW(TAG, "Restart setelah factory reset (%s)", device_id);

    // restart
    esp_restart();
}

// init
esp_err_t device_reset_init(void)
{
    return mqtt_service_register_command("RESET_DEVICE", reset_handler, NULL);
}
