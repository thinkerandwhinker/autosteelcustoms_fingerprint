/**
 * CONFIG_BUTTON_H
 *
 * behavior of BOOT button (GPIO 0) now depends on ESP mode:
 *
 * CONFIG_BUTTON_MODE_ENTER_CONFIG, available when ESP under operational mode
 * how to trigger?
 * hold 5 secs, then release => sets force_cfg and restarts into configuration mode
 *
 * CONFIG_BUTTON_MODE_RESET_LOGIN, availabel when ESP under configuratin mode to reset captive portal credentials
 * how to trigger?
 * hold 15 secs => set login creddntials to default value which has specified on secret files
 * affected every users who logged-in
 *
 * lohh? katanya buat sementara pake strapping pinnya? kok keterusan bang?
 * BACOTTT, ini udah jam 3 ya monyet, saya ngantuk. udah saya cari dan ga ketemu tombolnya
 */
#ifndef CONFIG_BUTTON_H
#define CONFIG_BUTTON_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CONFIG_BUTTON_POLL_MS 50
#define CONFIG_BUTTON_ENTER_CONFIG_HOLD_MS 5000
#define CONFIG_BUTTON_RESET_LOGIN_HOLD_MS 15000

    typedef enum
    {
        CONFIG_BUTTON_MODE_ENTER_CONFIG = 0,
        CONFIG_BUTTON_MODE_RESET_LOGIN,
    } config_button_mode_t;

    esp_err_t config_button_start(config_button_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif