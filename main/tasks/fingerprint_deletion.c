#include "fingerprint_deletion.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "as608.h"
#include "command_guard.h"
#include "mqtt_service.h"
#include "state_manager.h"
#include "ui_task.h"

static const char *TAG = "fp_delete";

#define CHANNEL_DELETION "deletion"

typedef struct
{
    bool success;
    bool already_empty;
    const char *reason; // DELETE_RESULT.reason, NULL on success
    const char *message;
} delete_outcome_t;

static delete_outcome_t failed(const char *reason, const char *message)
{
    delete_outcome_t o = {.success = false, .already_empty = false, .reason = reason, .message = message};
    return o;
}

/**
 * result builder + publihser
 * {
 *      event,
 *      requestId,
 *      fingerId,
 *      success,
 *      alreadyEmpty?,
 *      reason?,
 *      message?
 * }
 */
static void publish_result(const mqtt_command_t *cmd, uint16_t finger_id, const delete_outcome_t *o)
{
    cJSON *j = mqtt_event_new("DELETE_RESULT", cmd->request_id);
    if (j == NULL)
        return;
    cJSON_AddNumberToObject(j, "fingerId", finger_id);
    cJSON_AddBoolToObject(j, "success", o->success);
    if (o->already_empty)
        cJSON_AddBoolToObject(j, "alreadyEmpty", true);
    if (o->reason)
        cJSON_AddStringToObject(j, "reason", o->reason);
    if (o->message)
        cJSON_AddStringToObject(j, "message", o->message);
    mqtt_service_publish_json(CHANNEL_DELETION, j, 1);
    cJSON_Delete(j);
}

/**
 * remove template
 */
static delete_outcome_t delete_slot(uint16_t finger_id)
{
    // is sensor exist?
    if (!as608_is_ready())
        return failed("SENSOR_ERROR", "sensor sidik jari tidak siap");

    as608_status_t st = as608_check_slot_empty(finger_id);
    // is finger_id found?
    if (st == AS608_OK)
    {
        ESP_LOGW(TAG, "Slot %u sudah kosong sebelum dihapus", finger_id);
        delete_outcome_t o = {.success = true, .already_empty = true, .reason = NULL, .message = NULL};
        return o;
    }
    // is finger_id out of range?
    if (st == AS608_ERR_BAD_ID)
        return failed("INVALID_SLOT", "slot di luar kapasitas sensor");

    // is finger id being used by another process?
    if (st != AS608_ERR_SLOT_OCCUPIED)
    {
        ESP_LOGE(TAG, "Periksa slot %u: %s", finger_id, as608_status_str(st));
        return failed("SENSOR_ERROR", "gagal memeriksa slot");
    }

    // delete ONE slot (the driver always sends N = 1)
    const as608_status_t del = as608_delete_template(finger_id);
    // is deleted?
    if (del != AS608_OK)
        ESP_LOGW(TAG, "Hapus slot %u: %s, tetap diverifikasi", finger_id, as608_status_str(del));

    st = as608_check_slot_empty(finger_id);
    // deleted? fr?
    if (st == AS608_OK)
    {
        delete_outcome_t o = {.success = true, .already_empty = false, .reason = NULL, .message = NULL};
        return o;
    }
    if (st == AS608_ERR_SLOT_OCCUPIED)
    {
        // the sensor answered, it just did not delete: DELETE_FAILED. no answer at all: SENSOR_ERROR
        if (del == AS608_OK || del == AS608_ERR_ERASE_FAILED)
            return failed("DELETE_FAILED", "slot masih terisi setelah dihapus");
        return failed("SENSOR_ERROR", "sensor tidak merespons saat menghapus");
    }
    ESP_LOGE(TAG, "Verifikasi slot %u: %s", finger_id, as608_status_str(st));
    return failed("SENSOR_ERROR", "gagal memverifikasi slot");
}

/**
 * flow: guard => busy? => lock => delete => result => unlock
 */
static void delete_handler(const mqtt_command_t *cmd, void *ctx)
{
    (void)ctx;

    // is same req or expired?
    if (command_guard_check(cmd) != GUARD_OK)
    {
        return;
    }

    int64_t id = 0;
    if (!mqtt_command_get_int(cmd, "fingerId", 0, UINT16_MAX, &id))
    {
        ESP_LOGW(TAG, "DELETE_FINGERPRINT ditolak: fingerId tidak valid");
        return;
    }
    const uint16_t finger_id = (uint16_t)id;

    // is other command being executed?
    if (xEventGroupGetBits(sys_state_group) & DEVICE_BUSY_BIT)
    {
        ESP_LOGW(TAG, "DELETE_FINGERPRINT ditolak: perangkat sibuk");
        const delete_outcome_t busy = failed("BUSY", "perangkat sedang sibuk");
        publish_result(cmd, finger_id, &busy);
        return;
    }

    // lock
    xEventGroupSetBits(sys_state_group, DEVICE_BUSY_BIT);
    const delete_outcome_t o = delete_slot(finger_id);
    publish_result(cmd, finger_id, &o);

    if (o.success)
        // save last executed command
        command_guard_commit(cmd, false);

    if (o.success)
    {
        char line2[17];
        snprintf(line2, sizeof(line2), "Slot %u", finger_id);
        ui_feedback(UI_FEEDBACK_SUCCESS, "Jari Dihapus", line2);
    }
    else
    {
        ui_feedback(UI_FEEDBACK_FAILURE, "Hapus Gagal", o.reason);
    }
    xEventGroupClearBits(sys_state_group, DEVICE_BUSY_BIT);
}

// init
esp_err_t fingerprint_deletion_init(void)
{
    return mqtt_service_register_command("DELETE_FINGERPRINT", delete_handler, NULL);
}