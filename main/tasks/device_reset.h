/**
 * DEVICE_RESET_H
 *
 * handling for RESET_DEVICE command (factory reset)
 *
 * flow, eksekusinya wajib urutan
 * guard => busy? => keep device_id => empty sensor + prove it => erase NVS + prove it => RESET_REPORT (wait PUBACK) => FAREWELL (wait PUBACK) => stop mqtt => esp_restart()
 *
 *
 * ESP do PUBLISH into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/reset        1       RESET_REPORT (success / failure + actions),
 *                                          FAREWELL (last message of this device)
 *
 * ESP do SUBSCRIBE into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/commands     1       same router as RESTART, RESET_DEVICE is registered
 *                                          through mqtt_service_register_command()
 */

#ifndef DEVICE_RESET_H
#define DEVICE_RESET_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * register RESET_DEVICE
     */
    esp_err_t device_reset_init(void);

#ifdef __cplusplus
}
#endif

#endif
