/**
 * HEARTBEAT_H
 *
 * ping publisher & pong subscriber
 *
 * ESP do PUBLISH into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/ping         0       publish device info
 *
 * ESP do SUBSCRIBE into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/pong         0       listening for response from server
 */

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define HEARTBEAT_INTERVAL_MS 30000

    /**
     * ping pong handler
     *
     * must send ping when ESP start, and every 30s
     * handle incoming pong from backend
     */
    esp_err_t heartbeat_start_init(void);

#ifdef __cplusplus
}
#endif

#endif