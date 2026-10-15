/**
 * WEB_SERVER_H
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief running http server with captive protal, registering all web page, and star timer idle
     *
     * idle timer ensure device switch to OPERATIONAL MODE in case no activity within WEB_SERVER_IDLE_TIMEOUT_MS from http client
     */
    esp_err_t web_server_start(const esp_ip4_addr_t *ap_ip);

    void web_server_stop(void);

    /**
     * will call in every http request to reset idle timer
     */
    void web_server_touch_activity(void);

    /**
     * scheduled restart, waiting for respons send into client before ESP got restarted
     */
    void web_server_schedule_restart(uint32_t delay_ms);

#define WEB_SERVER_IDLE_TIMEOUT_MS (15u * 60u * 1000u)

#ifdef __cplusplus
}
#endif

#endif