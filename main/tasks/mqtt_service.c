#include "mqtt_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "nvs_config.h"
#include "secret.h"
#include "state_manager.h"
#include "validators.h"

static const char *TAG = "mqtt_svc";

#define MAX_SUBS 4
#define MAX_COMMANDS 8
#define RX_QUEUE_LEN 8
#define MAX_PAYLOAD 1024
#define TOPIC_MAX 80
#define CHANNEL_MAX 16
#define PUB_RING 8
#define MQTT_TASK_STACK 5120
#define MQTT_TASK_PRIORITY 4
#define MQTT_CLIENT_STACK 8192 // set bigger stack only for TLS handshake
#define PUB_DONE_BIT BIT0
#define TLS_PORT 8883 // mqtts

typedef struct
{
    char channel[CHANNEL_MAX];
    int qos;
    mqtt_message_handler_t handler;
    void *ctx;
    char topic[TOPIC_MAX];
} sub_t;

typedef struct
{
    char name[MQTT_CMD_MAX_NAME + 1];
    mqtt_command_handler_t handler;
    void *ctx;
} cmd_entry_t;

typedef struct
{
    char topic[TOPIC_MAX];
    char *payload;
    size_t len;
} rx_msg_t;

static esp_mqtt_client_handle_t s_client = NULL;
static QueueHandle_t s_rx_queue = NULL;
static EventGroupHandle_t s_pub_eg = NULL;
static SemaphoreHandle_t s_confirm_mutex = NULL;

static char s_device_id[MAX_LEN_DEVICE_ID + 1];
static sub_t s_subs[MAX_SUBS];
static int s_nsubs = 0;
static cmd_entry_t s_cmds[MAX_COMMANDS];
static int s_ncmds = 0;
static bool s_inited = false;
static bool s_started = false;
static volatile uint32_t s_connect_count = 0;

static int s_pub_ring[PUB_RING];
static int s_pub_pos = 0;
static portMUX_TYPE s_pub_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * utitlity function start from this line
 */
const char *mqtt_service_device_id(void) { return s_device_id; }
bool mqtt_service_is_connected(void) { return s_pub_eg && (xEventGroupGetBits(sys_state_group) & MQTT_CONNECTED_BIT); }
uint32_t mqtt_service_connect_count(void) { return s_connect_count; }

static esp_err_t build_topic(const char *channel, char *buf, size_t size)
{
    int n = snprintf(buf, size, "devices/%s/%s", s_device_id, channel);
    return (n > 0 && (size_t)n < size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

cJSON *mqtt_event_new(const char *event, const char *request_id)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL)
        return NULL;
    if (!cJSON_AddStringToObject(o, "event", event) ||
        (request_id && !cJSON_AddStringToObject(o, "requestId", request_id)))
    {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static void pub_ring_add(int msg_id)
{
    portENTER_CRITICAL(&s_pub_lock);
    s_pub_ring[s_pub_pos] = msg_id;
    s_pub_pos = (s_pub_pos + 1) % PUB_RING;
    portEXIT_CRITICAL(&s_pub_lock);
}

static bool pub_ring_has(int msg_id)
{
    bool found = false;
    portENTER_CRITICAL(&s_pub_lock);
    for (int i = 0; i < PUB_RING; i++)
    {
        if (s_pub_ring[i] == msg_id)
            found = true;
    }
    portEXIT_CRITICAL(&s_pub_lock);
    return found;
}

/**
 * registration function start from this line
 *
 * - subscribe func
 * - registrater for valid commands
 * - and other
 */
esp_err_t mqtt_service_subscribe(const char *channel, int qos, mqtt_message_handler_t handler, void *ctx)
{
    if (!s_inited || s_started)
        return ESP_ERR_INVALID_STATE;
    if (s_nsubs >= MAX_SUBS || channel == NULL || handler == NULL || strlen(channel) >= CHANNEL_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    sub_t *s = &s_subs[s_nsubs];
    strlcpy(s->channel, channel, sizeof(s->channel));
    s->qos = qos;
    s->handler = handler;
    s->ctx = ctx;
    esp_err_t err = build_topic(channel, s->topic, sizeof(s->topic));
    if (err != ESP_OK)
        return err;
    s_nsubs++;
    return ESP_OK;
}

esp_err_t mqtt_service_register_command(const char *name, mqtt_command_handler_t handler, void *ctx)
{
    if (!s_inited || s_started)
        return ESP_ERR_INVALID_STATE;
    if (s_ncmds >= MAX_COMMANDS || name == NULL || handler == NULL || strlen(name) > MQTT_CMD_MAX_NAME)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_ncmds; i++)
    {
        if (strcmp(s_cmds[i].name, name) == 0)
            return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_cmds[s_ncmds].name, name, sizeof(s_cmds[0].name));
    s_cmds[s_ncmds].handler = handler;
    s_cmds[s_ncmds].ctx = ctx;
    s_ncmds++;
    return ESP_OK;
}

/**
 * from this line, function is provide for mqtt task
 * including incoming command handler and unknown command rejection
 */
static void commands_message_handler(const char *payload, size_t len, void *ctx)
{
    (void)ctx;
    mqtt_command_t cmd;
    cJSON *root = NULL;

    mqtt_command_result_t r = mqtt_command_parse(payload, len, &cmd, &root);
    if (r != MQTT_CMD_OK)
    {
        ESP_LOGW(TAG, "Perintah ditolak: %s", mqtt_command_result_str(r));
        return;
    }

    for (int i = 0; i < s_ncmds; i++)
    {
        if (strcmp(s_cmds[i].name, cmd.command) == 0)
        {
            ESP_LOGI(TAG, "Perintah %s (requestId=%s)", cmd.command, cmd.request_id);
            s_cmds[i].handler(&cmd, s_cmds[i].ctx);
            cJSON_Delete(root);
            return;
        }
    }
    ESP_LOGW(TAG, "Perintah tidak dikenal diabaikan: %s", cmd.command);
    cJSON_Delete(root);
}

// unknown command rejection
static void dispatch(const rx_msg_t *m)
{
    for (int i = 0; i < s_nsubs; i++)
    {
        if (strcmp(m->topic, s_subs[i].topic) == 0)
        {
            s_subs[i].handler(m->payload, m->len, s_subs[i].ctx);
            return;
        }
    }
    ESP_LOGW(TAG, "Pesan pada topic tak terduga diabaikan: %s", m->topic);
}

/**
 * message is marked as event (esp-mqtt)
 * start from this line, incomming message will added to queue as task
 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Terhubung ke broker");
        s_connect_count++;
        for (int i = 0; i < s_nsubs; i++)
        {
            esp_mqtt_client_subscribe(e->client, s_subs[i].topic, s_subs[i].qos);
        }
        xEventGroupSetBits(sys_state_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Terputus dari broker");
        xEventGroupClearBits(sys_state_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_PUBLISHED:
        pub_ring_add(e->msg_id);
        xEventGroupSetBits(s_pub_eg, PUB_DONE_BIT);
        break;

    case MQTT_EVENT_DATA:
    {
        // is message too big?
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len ||
            e->data_len > MAX_PAYLOAD || e->topic == NULL || e->topic_len >= TOPIC_MAX)
        {
            ESP_LOGW(TAG, "Pesan dibuang (terlalu besar/terpotong)");
            break;
        }
        rx_msg_t m;
        memcpy(m.topic, e->topic, e->topic_len);
        m.topic[e->topic_len] = '\0';
        m.payload = malloc((size_t)e->data_len + 1);
        if (m.payload == NULL)
            break;
        memcpy(m.payload, e->data, e->data_len);
        m.payload[e->data_len] = '\0';
        m.len = (size_t)e->data_len;
        if (xQueueSend(s_rx_queue, &m, 0) != pdTRUE)
        {
            ESP_LOGW(TAG, "Queue penuh, pesan dibuang");
            free(m.payload);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        if (e->error_handle)
        {
            // is connection refused by broker?
            if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            {
                ESP_LOGE(TAG, "Koneksi ditolak broker (kode %d). 4/5 = username/password salah atau tidak berwenang",
                         e->error_handle->connect_return_code);
            }
            // is transport configuration error?
            else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            {
                ESP_LOGW(TAG, "Kesalahan transport (tls=0x%x, sock errno=%d)",
                         e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_transport_sock_errno);
            }
        }
        break;

    default:
        break;
    }
}

/**
 * publis function start from this line
 */
static int publish_raw(const char *channel, const char *payload, int qos)
{
    if (!s_started || s_client == NULL || !mqtt_service_is_connected())
        return -1;
    char topic[TOPIC_MAX];
    if (build_topic(channel, topic, sizeof(topic)) != ESP_OK)
        return -1;
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, 0);
}

esp_err_t mqtt_service_publish(const char *channel, const char *payload, int qos)
{
    if (!s_started || !mqtt_service_is_connected())
        return ESP_ERR_INVALID_STATE;
    return publish_raw(channel, payload, qos) >= 0 ? ESP_OK : ESP_FAIL;
}

// use mqtt_service_publish_json, instead of mqtt_service_publish, to publish JSON-formatted messages
esp_err_t mqtt_service_publish_json(const char *channel, const cJSON *obj, int qos)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (s == NULL)
        return ESP_ERR_NO_MEM;
    esp_err_t err = mqtt_service_publish(channel, s, qos);
    free(s);
    return err;
}

esp_err_t mqtt_service_publish_confirmed(const char *channel, const cJSON *obj, uint32_t timeout_ms)
{
    if (!s_started || !mqtt_service_is_connected())
        return ESP_ERR_INVALID_STATE;

    char *s = cJSON_PrintUnformatted(obj);
    if (s == NULL)
        return ESP_ERR_NO_MEM;

    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);
    esp_err_t result = ESP_ERR_TIMEOUT;

    if (xSemaphoreTake(s_confirm_mutex, total) != pdTRUE)
    {
        free(s);
        return ESP_ERR_TIMEOUT;
    }

    xEventGroupClearBits(s_pub_eg, PUB_DONE_BIT);
    int id = publish_raw(channel, s, 1);
    if (id <= 0)
    {
        result = ESP_FAIL;
    }
    else
    {
        for (;;)
        {
            if (pub_ring_has(id))
            {
                result = ESP_OK;
                break;
            }
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= total)
                break;
            xEventGroupWaitBits(s_pub_eg, PUB_DONE_BIT, pdTRUE, pdFALSE, total - elapsed);
        }
    }
    xSemaphoreGive(s_confirm_mutex);
    free(s);
    return result;
}

/**
 * start from this line, only providing for task initialization
 *
 * NOTE: JANGAN DI UBAH KALO GADA ERROR SPESIFIK DARI SINI
 */
static void mqtt_task(void *arg)
{
    (void)arg;
    bool running = false;

    for (;;)
    {
        const bool wifi = (xEventGroupGetBits(sys_state_group) & WIFI_CONNECTED_BIT) != 0;
        // is wifi connected?
        // is mqtt not running yet?
        if (wifi && !running)
        {
            ESP_LOGI(TAG, "WiFi siap, memulai klien MQTT");
            if (esp_mqtt_client_start(s_client) == ESP_OK)
                running = true;
        }
        else if (!wifi && running)
        {
            ESP_LOGW(TAG, "WiFi terputus, menghentikan klien MQTT");
            esp_mqtt_client_stop(s_client);
            xEventGroupClearBits(sys_state_group, MQTT_CONNECTED_BIT);
            running = false;
        }

        rx_msg_t m;
        if (xQueueReceive(s_rx_queue, &m, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            dispatch(&m);
            free(m.payload);
        }
    }
}

// initialization
esp_err_t mqtt_service_init(void)
{
    if (s_inited)
        return ESP_OK;

    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL)
        return ESP_ERR_NO_MEM;
    nvs_config_load(cfg);
    bool ok = validate_device_id(cfg->device_id, s_device_id);
    free(cfg);
    if (!ok)
    {
        ESP_LOGE(TAG, "device_id tidak valid");
        return ESP_ERR_INVALID_STATE;
    }

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_msg_t));
    s_pub_eg = xEventGroupCreate();
    s_confirm_mutex = xSemaphoreCreateMutex();
    if (!s_rx_queue || !s_pub_eg || !s_confirm_mutex)
        return ESP_ERR_NO_MEM;
    for (int i = 0; i < PUB_RING; i++)
        s_pub_ring[i] = -1;

    s_inited = true;
    return mqtt_service_subscribe("commands", 1, commands_message_handler, NULL);
}

esp_err_t mqtt_service_start(void)
{
    if (!s_inited || s_started)
        return ESP_ERR_INVALID_STATE;

    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL)
        return ESP_ERR_NO_MEM;
    nvs_config_load(cfg);

    const uint32_t port = (uint32_t)strtoul(cfg->mqtt_port, NULL, 10);
    const bool tls = (port == TLS_PORT);

    esp_mqtt_client_config_t mc = {
        .broker.address.hostname = cfg->mqtt_host,
        .broker.address.port = port,
        .broker.address.transport = tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = s_device_id,
        .credentials.username = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        .session.disable_clean_session = false, // activate clean session to avoid case diberondong message kayak alat skripsi saya dulu
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = MAX_PAYLOAD,
        .buffer.out_size = MAX_PAYLOAD,
        .task.stack_size = MQTT_CLIENT_STACK,
    };

    if (tls)
    {
#if defined(SECRET_MQTT_CA_PEM)
        // is CA CRET provided?
        if (sizeof(SECRET_MQTT_CA_PEM) > 1)
        {
            mc.broker.verification.certificate = SECRET_MQTT_CA_PEM;
            mc.broker.verification.certificate_len = sizeof(SECRET_MQTT_CA_PEM);
        }
        else
#endif
        {
            // in case CA CERT not provided, using built in CA CERT from esp idf
            mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    ESP_LOGI(TAG, "Broker %s:%u (%s), client_id=%s", cfg->mqtt_host, (unsigned)port,
             tls ? "TLS" : "TCP tanpa enkripsi", s_device_id);

    /**
     * NOTE: read for documentation about esp-mqtt, especially for esp_client_* function
     * at: https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32/api-reference/protocols/mqtt.html?highlight=esp_mqtt_client_init#_CPPv420esp_mqtt_client_initPK24esp_mqtt_client_config_t
     */

    // create client
    s_client = esp_mqtt_client_init(&mc);
    memset(cfg->mqtt_pass, 0, sizeof(cfg->mqtt_pass));
    free(cfg);
    if (s_client == NULL)
        return ESP_FAIL;

    // event handler mqtt
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK)
        return err;

    s_started = true;
    if (xTaskCreate(mqtt_task, "mqtt_task", MQTT_TASK_STACK, NULL, MQTT_TASK_PRIORITY, NULL) != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * gracefully stop mqtt task
 */
esp_err_t mqtt_service_stop(void)
{
    if (!s_started || s_client == NULL)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_mqtt_client_stop(s_client);
    xEventGroupClearBits(sys_state_group, MQTT_CONNECTED_BIT);
    return err;
}