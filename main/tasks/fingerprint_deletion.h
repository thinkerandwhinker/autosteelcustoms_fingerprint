/**
 * FINGERPRINT_DELETION_H
 *
 * handling for DELETE_FINGERPRINT command
 *
 * flow: guard => busy? => lock => slot already empty? => delete ONE slot => look again => result
 * success:true always means "the slot was PROVEN empty after the command", never "the command was sent"
 *
 * ESP do PUBLISH into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/deletion     1       DELETE_RESULT
 *
 * ESP do SUBSCRIBE into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/commands     1       same router as RESTART, DELETE_FINGERPRINT is registered
 *                                          through mqtt_service_register_command()
 */

#ifndef FINGERPRINT_DELETION_H
#define FINGERPRINT_DELETION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * register DELETE_FINGERPRINT
     */
    esp_err_t fingerprint_deletion_init(void);

#ifdef __cplusplus
}
#endif

#endif
