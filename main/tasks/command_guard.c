#include "command_guard.h"

#include <string.h>

#include "esp_log.h"

#include "clock_sync.h"
#include "nvs_config.h"

static const char *TAG = "cmd_guard";

#define SEEN_RING 8

static char s_seen[SEEN_RING][MQTT_CMD_MAX_REQUEST_ID + 1];
static int s_seen_pos = 0;

/**
 * check current requestId
 */
static bool seen_in_ring(const char *rid)
{
    for (int i = 0; i < SEEN_RING; i++)
    {
        if (s_seen[i][0] != '\0' && strcmp(s_seen[i], rid) == 0)
            return true;
    }
    return false;
}

/**
 * validate command
 */
guard_result_t command_guard_check(const mqtt_command_t *cmd)
{
    // is request has in list of processed request?
    if (seen_in_ring(cmd->request_id))
    {
        ESP_LOGW(TAG, "%s diabaikan: requestId sudah pernah diproses", cmd->command);
        return GUARD_DUPLICATE;
    }

    char last[MAX_LEN_REQUEST_ID + 1];

    // is new incoming command has processed before?
    if (nvs_config_get_last_request(last, sizeof(last)) == ESP_OK && last[0] != '\0' &&
        strcmp(last, cmd->request_id) == 0)
    {
        ESP_LOGW(TAG, "%s diabaikan: requestId sama dengan perintah terakhir yang dieksekusi", cmd->command);
        return GUARD_DUPLICATE;
    }

    if (cmd->has_ttl)
    {
        int64_t now;
        if (!cmd->has_issued_at)
        {
            ESP_LOGW(TAG, "%s: ttlMs tanpa issuedAt, hanya requestId yang diandalkan", cmd->command);
        }
        else if (!clock_sync_now_ms(&now))
        {
            ESP_LOGW(TAG, "%s: jam belum sinkron, hanya requestId yang diandalkan", cmd->command);
        }
        // is request expired?
        else if (now - cmd->issued_at_ms > cmd->ttl_ms)
        {
            ESP_LOGW(TAG, "%s diabaikan: kedaluwarsa (umur %lld ms > ttl %lld ms)", cmd->command,
                     (long long)(now - cmd->issued_at_ms), (long long)cmd->ttl_ms);
            return GUARD_STATE;
        }
    }
    return GUARD_OK;
}

/**
 * save requestId as last processed request
 */
bool command_guard_commit(const mqtt_command_t *cmd, bool persist)
{
    strlcpy(s_seen[s_seen_pos], cmd->request_id, sizeof(s_seen[0]));
    s_seen_pos = (s_seen_pos + 1) % SEEN_RING;

    if (!persist)
        return true;
    esp_err_t err = nvs_config_set_last_request(cmd->request_id);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Gagal menyimpan requestId ke NVS: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}