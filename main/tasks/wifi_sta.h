/**
 * WIFI_STA_H
 */
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_MAX_CONSECUTIVE_FAILURES 10 // max retry attempt

#define WIFI_RETRY_BASE_DELAY_MS 2000u // retry interval first
#define WIFI_RETRY_MAX_DELAY_MS 30000u // retry interval max, constant

    /**
     * @brief turn on wifi station using saved credential from NVS
     *
     * - GOT_IP       : set WIFI_CONNECTED_BIT, failed attempt count
     *                  when at first time ESP success to connect, credential will mark as verified
     * - DISCONNECTED : clear WIFI_CONNECTED_BIT, count failed attempt and retry from base interval, backoff untill max interval
     *
     * backup term (call for mode_manager_enter_config()): set force_cfg then restart
     *   - credenial not verified : when max attempt reached
     *   - credential verified : only set when max attempt reached but failed got known error
     */
    esp_err_t wifi_sta_start(void);

#ifdef __cplusplus
}
#endif

#endif