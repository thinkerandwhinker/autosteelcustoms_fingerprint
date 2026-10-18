/**
 * DEVICE_RESTART_H
 *
 * handling for restart command
 *
 * ESP do PUBLISH into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/restart      0       publish restart command status,
 *                                          steps & rejection reason
 *
 * ESP do SUBSCRIBE into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/command      0       command source topic
 */

#ifndef DEVICE_RESTART_H
#define DEVICE_RESTART_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * init
     */
    esp_err_t device_restart_init(void);

#ifdef __cplusplus
}
#endif

#endif