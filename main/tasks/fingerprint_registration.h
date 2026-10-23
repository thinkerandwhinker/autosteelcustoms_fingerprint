/**
 * FINGERPRINT_REGISTRATION_H
 *
 * handling for fingerprint registration command
 *
 * the enrollment runs in its OWN task (fp_register), the command handlers only start / cancel it,
 * so mqtt_task is never blocked while the user is putting a finger on the sensor
 *
 *   START_REGISTER  => REGISTER_ACK (right away) => lock (DEVICE_BUSY_BIT + MODE_REGISTRATION_BIT)
 *                   => task: check sensor + slot => REGISTER_READY
 *                   => REGISTER_PROGRESS PLACE_FINGER => REMOVE_FINGER => PLACE_AGAIN => PROCESSING
 *                   => save into the slot given by backend => REGISTER_SUCCESS(fingerId)
 *                   => unlock. any failure => REGISTER_FAILED(reason) => unlock
 *   CANCEL_REGISTER => stop the task right away, NO event is sent, nothing is kept in the sensor
 *
 * while locked, RESTART / RESET_DEVICE / DELETE_FINGERPRINT are rejected with BUSY
 *
 * ESP do PUBLISH into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                       qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/registration    1       REGISTER_ACK / READY / PROGRESS / SUCCESS / FAILED
 *
 * ESP do SUBSCRIBE into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                       qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/commands        1       same router as RESTART
 */

#ifndef FINGERPRINT_REGISTRATION_H
#define FINGERPRINT_REGISTRATION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * trigger: START_REGISTER, CANCEL_REGISTER
     */
    esp_err_t fingerprint_registration_init(void);

#ifdef __cplusplus
}
#endif

#endif
