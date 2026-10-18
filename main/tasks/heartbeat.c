#include "heartbeat.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "clock_sync.h"
#include "mqtt_service.h"

static const char *TAG = "heartbeat";

#define HEARTBEAT_STACK 3072
#define HEARTBEAT_PRIORITY 3
#define FIRMWARE_MAX_LEN 32

/**
 * send ping ESP status
 */
static void send_ping(void)
{
    cJSON *payload = cJSON_CreateObject();

    // is payload created?
    if (payload == NULL)
        return;

    char fw[FIRMWARE_MAX_LEN + 1];
    const esp_app_desc_t *d = esp_app_get_description();
    strlcpy(fw, d ? d->version : "", sizeof(fw));

    cJSON_AddNumberToObject(
        payload,
        "uptimeMs", // uptime field since ESP got turned on
        (double)clock_uptime_ms());

    cJSON_AddStringToObject(
        payload,
        "firmware", // firmware version field
        fw);

    // publish ping
    esp_err_t err = mqtt_service_publish_json(
        "ping",
        payload,
        0);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Ping gagal dikirim: %s", esp_err_to_name(err));
    }

    // clear object buffer
    cJSON_Delete(payload);
}

// handler incoming pong mesage from backend
static void pong_handler(const char *payload, size_t len, void *ctx)
{
    (void)ctx;

    // parse into JSON
    cJSON *root = cJSON_ParseWithLength(payload, len);

    // is payload valid (aka not NULL)?
    if (root == NULL)
        return;

    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "serverTimeUtc");

    int64_t epoch_ms;

    if (cJSON_IsString(ev) && ev->valuestring && strcmp(ev->valuestring, "PONG") == 0 && cJSON_IsString(ts) && ts->valuestring && clock_parse_iso8601_utc(ts->valuestring, &epoch_ms))
    {
        clock_sync_set_epoch_ms(epoch_ms);
        ESP_LOGD(TAG, "Jam disinkronkan dari PONG");
    }
    // clear object buffer
    cJSON_Delete(root);
}

// ping task with constant intervals to backend
static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t last_connect = 0;

    for (;;)
    {
        while (!mqtt_service_is_connected() || mqtt_service_connect_count() == last_connect)
        {
            // wait till connect to broker
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        last_connect = mqtt_service_connect_count();

        // immediately send ping into backend when it's connected
        send_ping();

        uint32_t waited = 0;

        while (mqtt_service_is_connected() && mqtt_service_connect_count() == last_connect)
        {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
            // is gte 30s?
            if (waited >= HEARTBEAT_INTERVAL_MS)
            {
                // ping backend again
                send_ping();
                waited = 0;
            }
        }
    }
}

// registering ping/pong
esp_err_t heartbeat_start_init(void)
{
    esp_err_t err = mqtt_service_subscribe(
        "pong",
        0, // ga penting penting amat, kadang butuh. Jaga jaga aja kalo RTC ku rusak
        pong_handler,
        NULL);

    if (err != ESP_OK)
        return err;

    // is can't add task?
    if (xTaskCreate(heartbeat_task, "heartbeat", HEARTBEAT_STACK, NULL, HEARTBEAT_PRIORITY, NULL) != pdPASS)
    {
        // [MARKED]
        // biasanya gara-gara memori penuh, besok tak baca lagi dokumentasinya
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}