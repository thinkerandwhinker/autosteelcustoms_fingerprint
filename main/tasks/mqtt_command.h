/**
 * MQTT_COMMAND_H
 *
 * parse every incoming command from devices/{device_id}/commands
 * expected incomming command message format, bases on backend contract:
 * {
 *      "command": "...", // what to do?
 *      "requestId": "...", // who is it?
 *      "issuedAt": <epoch ms>, // when?
 *      "ttlMs": <ms>, // deadline?
 *      ...other
 * }
 *
 */

#ifndef MQTT_COMMAND_H
#define MQTT_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MQTT_CMD_MAX_NAME 32       // max length of command
#define MQTT_CMD_MAX_REQUEST_ID 64 // max length of requestId

    typedef struct
    {
        char command[MQTT_CMD_MAX_NAME + 1];
        char request_id[MQTT_CMD_MAX_REQUEST_ID + 1];
        bool has_issued_at;
        int64_t issued_at_ms;
        bool has_ttl;
        int64_t ttl_ms;
        const cJSON *root; // for command with additional parameters
    } mqtt_command_t;

    typedef enum
    {
        MQTT_CMD_OK = 0,
        MQTT_CMD_ERR_NOT_JSON,
        MQTT_CMD_ERR_NOT_OBJECT,
        MQTT_CMD_ERR_NO_COMMAND,
        MQTT_CMD_ERR_BAD_REQUEST_ID,
    } mqtt_command_result_t;

    /**
     * JSON parsing
     */
    mqtt_command_result_t mqtt_command_parse(const char *payload, size_t len, mqtt_command_t *out, cJSON **root_out);

    /**
     * extract command from payload and return it as string
     */
    const char *mqtt_command_result_str(mqtt_command_result_t r);

    /**
     * read an integer parameter of the command payload (e.g. "fingerId")
     *
     * @return true only when the key exists, is a whole number and min <= value <= max
     *         `out` is untouched otherwise
     */
    bool mqtt_command_get_int(const mqtt_command_t *cmd, const char *key, int64_t min, int64_t max, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif