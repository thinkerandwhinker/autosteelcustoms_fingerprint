#include "fingerprint_registration.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "as608.h"
#include "command_guard.h"
#include "mqtt_service.h"
#include "state_manager.h"
#include "ui_task.h"

static const char *TAG = "fp_register";

#define CHANNEL_REGISTRATION "registration"

#define DEFAULT_TIMEOUT_MS 60000 // contract: START_REGISTER.timeoutMs
#define MIN_TIMEOUT_MS 5000
#define MAX_TIMEOUT_MS 300000
#define TASK_STACK 6144
#define TASK_PRIORITY 3         // below mqtt_task
#define MAX_CAPTURE_ATTEMPTS 3  // chance when image too burik
#define SETTLE_TIMEOUT_MS 3000  // after success: wait until the finger leaves the sensor
#define PUBLISH_TIMEOUT_MS 3000 // final events wait for PUBACK, a lost SUCCESS means an orphan template
#define PUBLISH_TRIES 3
#define PUBLISH_RETRY_DELAY_MS 300

static struct
{
    volatile bool active;
    volatile bool cancel;
    char request_id[MQTT_CMD_MAX_REQUEST_ID + 1];
    uint16_t finger_id;
    uint32_t timeout_ms;
    TickType_t start;
} s;

/**
 * report event handler start from this line
 */
static void send_simple(const char *event, const char *request_id)
{
    cJSON *o = mqtt_event_new(event, request_id);
    if (o == NULL)
        return;
    mqtt_service_publish_json(CHANNEL_REGISTRATION, o, 1);
    cJSON_Delete(o);
}

static void send_progress(const char *step)
{
    cJSON *o = mqtt_event_new("REGISTER_PROGRESS", s.request_id);
    if (o == NULL)
        return;
    cJSON_AddStringToObject(o, "step", step);
    mqtt_service_publish_json(CHANNEL_REGISTRATION, o, 1);
    cJSON_Delete(o);
}

/**
 * wait PUBACK do try a few times
 */
static esp_err_t send_final(cJSON *o)
{
    esp_err_t err = ESP_ERR_NO_MEM;
    // retry publish
    for (int i = 0; o != NULL && i < PUBLISH_TRIES; i++)
    {
        err = mqtt_service_publish_confirmed(CHANNEL_REGISTRATION, o, PUBLISH_TIMEOUT_MS);
        // is publish received?
        if (err == ESP_OK)
            break;
        ESP_LOGW(TAG, "Publish belum terkonfirmasi (%s), percobaan %d/%d", esp_err_to_name(err), i + 1, PUBLISH_TRIES);
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_RETRY_DELAY_MS));
    }
    cJSON_Delete(o);
    return err;
}

static cJSON *failed_event(const char *request_id, const char *reason, const char *message)
{
    cJSON *o = mqtt_event_new("REGISTER_FAILED", request_id);
    if (o == NULL)
        return NULL;
    cJSON_AddStringToObject(o, "reason", reason);
    if (message)
        cJSON_AddStringToObject(o, "message", message);
    return o;
}

/**
 * driver status => REGISTER_FAILED.reason
 *
 * baru ngeh kalau logika matching template as608 bisa dipake buat cek temolat yang duplikasi juga,
 * sementara message dibuat spesifik, tapi pengelompokannya ke 'UNKNOWN'
 */
static const char *classify(as608_status_t st, uint16_t duplicate_id, char *msg, size_t msg_size)
{
    switch (st)
    {
    case AS608_ERR_TIMEOUT:
        snprintf(msg, msg_size, "jari tidak terdeteksi sebelum waktu habis");
        return "SCAN_TIMEOUT";
    case AS608_ERR_MISMATCH:
        snprintf(msg, msg_size, "jari kedua tidak cocok dengan jari pertama");
        return "FINGER_MISMATCH";
    case AS608_ERR_POOR_IMAGE:
    case AS608_ERR_IMAGE_FAIL:
        snprintf(msg, msg_size, "kualitas gambar jari buruk");
        return "POOR_IMAGE";
    case AS608_ERR_DUPLICATE:
        snprintf(msg, msg_size, "jari sudah terdaftar di slot %u", duplicate_id);
        return "UNKNOWN";
    case AS608_ERR_SLOT_OCCUPIED:
        snprintf(msg, msg_size, "slot %u sudah terisi di sensor (template yatim?)", s.finger_id);
        return "STORAGE_ERROR";
    case AS608_ERR_BAD_ID:
        snprintf(msg, msg_size, "slot %u di luar kapasitas sensor", s.finger_id);
        return "STORAGE_ERROR";
    case AS608_ERR_FLASH:
        snprintf(msg, msg_size, "sensor gagal menulis template");
        return "STORAGE_ERROR";
    case AS608_ERR_NO_RESPONSE:
        snprintf(msg, msg_size, "sensor tidak merespons");
        return "SENSOR_ERROR";
    case AS608_ERR_COMM:
    case AS608_ERR_WRONG_PASSWORD:
    case AS608_ERR_INTERNAL:
    case AS608_ERR_ERASE_FAILED:
    case AS608_ERR_SENSOR:
        snprintf(msg, msg_size, "kesalahan sensor (kode 0x%02x)", as608_last_confirm_code());
        return "SENSOR_ERROR";
    default:
        snprintf(msg, msg_size, "kesalahan internal (%s)", as608_status_str(st));
        return "UNKNOWN";
    }
}

/**
 * registration time left
 */
static uint32_t remaining_ms(void)
{
    const TickType_t elapsed = xTaskGetTickCount() - s.start;
    const TickType_t total = pdMS_TO_TICKS(s.timeout_ms);
    return (elapsed >= total) ? 0 : (uint32_t)((total - elapsed) * portTICK_PERIOD_MS);
}

/**
 * is timeout in N step?
 */
static as608_status_t do_step(as608_enroll_step_t step, uint16_t *duplicate_id)
{
    if (s.cancel)
        return AS608_ERR_CANCELLED;
    const uint32_t left = remaining_ms();
    if (left == 0)
        return AS608_ERR_TIMEOUT;
    return as608_enroll_step(step, left, duplicate_id);
}

/**
 * capture
 */
static as608_status_t capture_step(as608_enroll_step_t step, const char *step_name, uint16_t *duplicate_id)
{
    for (int attempt = 1;; attempt++)
    {
        send_progress(step_name);
        as608_status_t st = do_step(step, duplicate_id);
        if ((st != AS608_ERR_POOR_IMAGE && st != AS608_ERR_IMAGE_FAIL) || attempt >= MAX_CAPTURE_ATTEMPTS)
            return st;

        ESP_LOGW(TAG, "%s: %s, coba lagi (%d/%d)", step_name, as608_status_str(st), attempt, MAX_CAPTURE_ATTEMPTS);
        send_progress("REMOVE_FINGER");
        st = do_step(AS608_STEP_REMOVE_FINGER, NULL);
        if (st != AS608_OK)
            return st;
    }
}

/**
 * do "PING" to as608
 */
static as608_status_t prepare(void)
{
    // is sensor die?
    if (!as608_is_ready())
        return AS608_ERR_NO_RESPONSE;

    uint16_t count = 0;
    as608_status_t st = as608_template_count(&count); // "are you there?"
    if (st != AS608_OK)
        return st;
    return as608_check_slot_empty(s.finger_id); // OK = empty, SLOT_OCCUPIED, BAD_ID
}

static as608_status_t enroll_and_store(uint16_t *duplicate_id)
{
    as608_status_t st = prepare();
    if (st != AS608_OK)
        return st;
    send_simple("REGISTER_READY", s.request_id);

    st = capture_step(AS608_STEP_PLACE_FINGER, "PLACE_FINGER", duplicate_id);
    if (st != AS608_OK)
        return st;

    send_progress("REMOVE_FINGER");
    st = do_step(AS608_STEP_REMOVE_FINGER, NULL);
    if (st != AS608_OK)
        return st;

    st = capture_step(AS608_STEP_PLACE_AGAIN, "PLACE_AGAIN", NULL);
    if (st != AS608_OK)
        return st;

    send_progress("PROCESSING");
    if (s.cancel)
        return AS608_ERR_CANCELLED;
    return as608_save_template(s.finger_id); // merging & saving
}

/**
 * rollback
 *
 * kick registered finger on sensor when backend doesn't send SUCCESS response after registration
 */
static void rollback_template(const char *why)
{
    const as608_status_t st = as608_delete_template(s.finger_id);
    if (st == AS608_OK)
        ESP_LOGW(TAG, "Template slot %u ditarik kembali: %s", s.finger_id, why);
    else
        ESP_LOGE(TAG, "Template slot %u yatim, gagal ditarik kembali (%s): %s", s.finger_id, as608_status_str(st), why);
}

static void complete_success(void)
{
    // cancelled by backend?
    if (s.cancel)
    {
        rollback_template("dibatalkan sebelum SUCCESS dikirim");
        return;
    }

    cJSON *o = mqtt_event_new("REGISTER_SUCCESS", s.request_id);
    if (o != NULL)
        cJSON_AddNumberToObject(o, "fingerId", s.finger_id);
    const esp_err_t err = send_final(o);

    // does not receive pubback yet or got cancel?
    if (err != ESP_OK || s.cancel)
    {
        rollback_template(err != ESP_OK ? "SUCCESS tidak terkonfirmasi" : "dibatalkan saat SUCCESS dikirim");
        return;
    }

    ESP_LOGI(TAG, "Registrasi selesai: slot %u", s.finger_id);
    ui_feedback(UI_FEEDBACK_SUCCESS, "Jari Terdaftar", "");

    const as608_status_t st = as608_enroll_step(AS608_STEP_REMOVE_FINGER, SETTLE_TIMEOUT_MS, NULL);
    // second scan finger belum diangkat from sensor
    if (st != AS608_OK)
        ESP_LOGW(TAG, "Jari belum diangkat: %s", as608_status_str(st));
}

static void report_failure(as608_status_t st, uint16_t duplicate_id)
{
    char msg[96];
    const char *reason = classify(st, duplicate_id, msg, sizeof(msg));
    ESP_LOGE(TAG, "Registrasi gagal: %s => %s (%s)", as608_status_str(st), reason, msg);

    send_final(failed_event(s.request_id, reason, msg));
    ui_feedback(UI_FEEDBACK_FAILURE, "Registrasi Gagal", reason);
}

static void registration_task(void *arg)
{
    (void)arg;
    uint16_t duplicate_id = 0;
    const as608_status_t st = enroll_and_store(&duplicate_id);

    if (st == AS608_OK)
        complete_success();
    else if (st == AS608_ERR_CANCELLED)
        ESP_LOGW(TAG, "Registrasi dibatalkan (requestId=%s), tanpa event", s.request_id); // backend already left
    else
        report_failure(st, duplicate_id);

    // unlock, back to idle
    ESP_LOGI(TAG, "Sisa stack minimum fp_register: %u byte", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s.active = false;
    // delete task
    xEventGroupClearBits(sys_state_group, MODE_REGISTRATION_BIT | DEVICE_BUSY_BIT);
    vTaskDelete(NULL);
}

/**
 * command handlers
 */
static void start_handler(const mqtt_command_t *cmd, void *ctx)
{
    (void)ctx;

    // is same req? (an old START_REGISTER must never start a registration nobody waits for)
    if (command_guard_check(cmd) != GUARD_OK)
    {
        return;
    }

    command_guard_commit(cmd, false);

    // command received
    send_simple("REGISTER_ACK", cmd->request_id);

    int64_t finger_id = 0;
    int64_t timeout_ms = DEFAULT_TIMEOUT_MS;
    if (!mqtt_command_get_int(cmd, "fingerId", 0, UINT16_MAX, &finger_id))
    {
        ESP_LOGW(TAG, "START_REGISTER ditolak: fingerId tidak valid");
        cJSON *o = failed_event(cmd->request_id, "UNKNOWN", "fingerId tidak valid");
        if (o)
        {
            mqtt_service_publish_json(CHANNEL_REGISTRATION, o, 1);
            cJSON_Delete(o);
        }
        return;
    }
    if (!mqtt_command_get_int(cmd, "timeoutMs", MIN_TIMEOUT_MS, MAX_TIMEOUT_MS, &timeout_ms))
        timeout_ms = DEFAULT_TIMEOUT_MS;

    // is other command being executed?
    if (xEventGroupGetBits(sys_state_group) & DEVICE_BUSY_BIT)
    {
        ESP_LOGW(TAG, "START_REGISTER ditolak: perangkat sibuk");
        cJSON *o = failed_event(cmd->request_id, "UNKNOWN", "BUSY: perangkat sedang sibuk");
        if (o)
        {
            mqtt_service_publish_json(CHANNEL_REGISTRATION, o, 1);
            cJSON_Delete(o);
        }
        return;
    }

    // lock task MODE_REGISTRATION_BIT
    xEventGroupSetBits(sys_state_group, MODE_REGISTRATION_BIT | DEVICE_BUSY_BIT);
    s.cancel = false;
    strlcpy(s.request_id, cmd->request_id, sizeof(s.request_id));
    s.finger_id = (uint16_t)finger_id;
    s.timeout_ms = (uint32_t)timeout_ms;
    s.start = xTaskGetTickCount();
    s.active = true;
    ESP_LOGI(TAG, "Registrasi dimulai: slot %u, batas %lu ms (requestId=%s)", s.finger_id,
             (unsigned long)s.timeout_ms, s.request_id);
    ui_feedback(UI_FEEDBACK_SUCCESS, "Registrasi Jari", "Dimulai");

    // create enrollment task
    if (xTaskCreate(registration_task, "fp_register", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Gagal membuat task registrasi");
        s.active = false;
        xEventGroupClearBits(sys_state_group, MODE_REGISTRATION_BIT | DEVICE_BUSY_BIT);
        cJSON *o = failed_event(cmd->request_id, "UNKNOWN", "gagal membuat task registrasi");
        if (o)
        {
            mqtt_service_publish_json(CHANNEL_REGISTRATION, o, 1);
            cJSON_Delete(o);
        }
    }
}

/**
 * fingerprint registration cancellation command handler
 */
static void cancel_handler(const mqtt_command_t *cmd, void *ctx)
{
    (void)ctx;

    // is registration task active or request id same with current registration task?
    if (!s.active || strcmp(s.request_id, cmd->request_id) != 0)
    {
        ESP_LOGI(TAG, "CANCEL_REGISTER diabaikan: tidak ada registrasi aktif dengan requestId ini");
        return;
    }

    ESP_LOGW(TAG, "CANCEL_REGISTER diterima (requestId=%s)", cmd->request_id);
    s.cancel = true;
    as608_cancel();
}

// init
esp_err_t fingerprint_registration_init(void)
{
    esp_err_t err = mqtt_service_register_command("START_REGISTER", start_handler, NULL);
    if (err == ESP_OK)
        err = mqtt_service_register_command("CANCEL_REGISTER", cancel_handler, NULL);
    return err;
}
