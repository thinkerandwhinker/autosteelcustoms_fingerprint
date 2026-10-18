/**
 * COMMAND_GUARD_H
 *
 * command security, intercept all unregistered command
 *
 * - checking fo duplicate command with same requesId
 *   ESP also save last requestId on NVS
 * - expired command checking using issuedAt & ttlMs
 *   ESP clock sync using PONG response from backend
 */

#ifndef COMMAND_GUARD_H
#define COMMAND_GUARD_H

#include <stdbool.h>
#include "mqtt_command.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        GUARD_OK = 0,
        GUARD_DUPLICATE,
        GUARD_STATE,
    } guard_result_t;

    /**
     * checking comamnd validity
     */
    guard_result_t command_guard_check(const mqtt_command_t *cmd);

    /**
     * mark processed command, persist set to true and commit requestId into NVS
     */
    bool command_guard_commit(const mqtt_command_t *cmd, bool persist);

#ifdef __cplusplus
}
#endif

#endif
