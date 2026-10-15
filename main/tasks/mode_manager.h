/**
 * MODE_MANAGER_H
 *
 * the only way to set ESP mode into OPERATIONAL MODE or CONFIG MODE
 */

#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * CONFIG MODE
     *
     * tasks that can be executed in this mode:
     * AP WPA2, Web Server (login), DNS hijack, captive portal credential reset
     */
    void start_config_mode(void);

    /**
     * OPERATIONAL MODE
     *
     * tasks that can be executed in this mode:
     * WiFi STA, entering config mode button (BOOT/GPIO 0)
     * sisanya nyusul...
     */
    void start_operational_mode(void);

    /**
     * @brief the only way to entering CONFIG MODE when ESP current mode is OPERATIONAL MODE
     *
     * what is this thing going to do?
     *
     * set the force_cfg flag in NVS (one time use) and then restart.
     * option to wipe the SSID is no longer being used, Wi-Fi credentials remain stored and are displayed on the web configuration page (biar kayak router indihome)
     * next boot, ESP enter configuration mode, and the flag is deleted at that same moment
     * doesn't return when success
     * returns an error if NVS fails (ESP continue to operate) or ESP_ERR_INVALID_STATE if already running
     *
     * @param reason reason for log
     * @param user_initiated true = user action feedback success, false = failed
     */
    esp_err_t mode_manager_enter_config(const char *reason, bool user_initiated);

#ifdef __cplusplus
}
#endif

#endif