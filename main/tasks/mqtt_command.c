#include "mqtt_command.h"

#include <math.h>
#include <string.h>

// ensure incoming command is valid ASCII
static bool printable_token(const char *s, size_t max_len)
{
    size_t n = strlen(s);

    // is it not null or not too big?
    if (n == 0 || n > max_len)
        return false;

    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;

        // is it spaces or control char?
        if (c < 0x21 || c == 0x7F)
            return false;
    }
    return true;
}

static bool read_ms(const cJSON *root, const char *key, int64_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(it))
        return false;
    double v = it->valuedouble;
    if (!isfinite(v) || v < 0 || v > 4e15)
        return false;
    *out = (int64_t)v;
    return true;
}

mqtt_command_result_t mqtt_command_parse(const char *payload, size_t len, mqtt_command_t *out, cJSON **root_out)
{
    *root_out = NULL;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL)
        return MQTT_CMD_ERR_NOT_JSON;
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return MQTT_CMD_ERR_NOT_OBJECT;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL || !printable_token(cmd->valuestring, MQTT_CMD_MAX_NAME))
    {
        cJSON_Delete(root);
        return MQTT_CMD_ERR_NO_COMMAND;
    }

    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (!cJSON_IsString(rid) || rid->valuestring == NULL || !printable_token(rid->valuestring, MQTT_CMD_MAX_REQUEST_ID))
    {
        cJSON_Delete(root);
        return MQTT_CMD_ERR_BAD_REQUEST_ID;
    }

    strlcpy(out->command, cmd->valuestring, sizeof(out->command));
    strlcpy(out->request_id, rid->valuestring, sizeof(out->request_id));
    out->has_issued_at = read_ms(root, "issuedAt", &out->issued_at_ms);
    out->has_ttl = read_ms(root, "ttlMs", &out->ttl_ms) && out->ttl_ms > 0;
    out->root = root;
    *root_out = root;
    return MQTT_CMD_OK;
}

const char *mqtt_command_result_str(mqtt_command_result_t r)
{
    switch (r)
    {
    case MQTT_CMD_OK:
        return "ok";
    case MQTT_CMD_ERR_NOT_JSON:
        return "bukan JSON";
    case MQTT_CMD_ERR_NOT_OBJECT:
        return "bukan objek JSON";
    case MQTT_CMD_ERR_NO_COMMAND:
        return "field command tidak valid";
    case MQTT_CMD_ERR_BAD_REQUEST_ID:
        return "field requestId tidak valid";
    }
    return "?";
}

bool mqtt_command_get_int(const mqtt_command_t *cmd, const char *key, int64_t min, int64_t max, int64_t *out)
{
    if (cmd == NULL || cmd->root == NULL || key == NULL || out == NULL)
        return false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(cmd->root, key);
    if (!cJSON_IsNumber(it))
        return false;
    const double v = it->valuedouble;
    // is nan, is out of range?
    if (!(v >= (double)min && v <= (double)max) || v != (double)(int64_t)v)
        return false;
    *out = (int64_t)v;
    return true;
}