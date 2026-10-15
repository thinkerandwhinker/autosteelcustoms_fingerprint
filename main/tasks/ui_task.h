/**
 * UI_TASK_H
 *
 * UI indicator components:
 * - green led
 * - blue led
 * - yellow led
 * - buzzer 3.3V
 * - lcd 16x2
 *
 * primary status is now obtained automatically from sys_state_group:
 *   MODE_CONFIG  => blue led blink, lcd display "Mode Konfigurasi"
 *   DEVICE_BUSY  => lcd display "Memproses..."
 *   !WIFI        => lcd display "Menunggu WiFi..."
 *   !MQTT        => lcd display "Menunggu MQTT..."
 *   siap         => blue led static, lcd "Siap Absen"
 *
 * instant feedback for other processes sent via ui_feedback()
 */
#ifndef UI_TASK_H
#define UI_TASK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        UI_FEEDBACK_SUCCESS = 0, // green led HIGH 1 secs, 1x short beep
        UI_FEEDBACK_FAILURE,     // yellow led HIGH 1 secs, 3x quick beep
    } ui_feedback_t;

    /**
     * @brief initialize GPIO, I2C bus, queue, and running ui_task
     *        called after sys_state_group has created and MODE_CONFIG_BIT has defined
     *        lcd 16x2 is optional, if no lcd, ga masalah
     *        masih bisa kasih feedback pake led sama buzzer
     */
    esp_err_t ui_task_start(void);

    /**
     * @brief trigger instant feedback, non-blocking, idempt, modular
     * @param line1 lcd line 1 max 16 char, can be null
     * @param line2 lcd line 2 max 16 char, can be null
     * @return if ui_task not running yet: ESP_OK, ESP_ERR_INVALID_STATE
     *         if queue is full = ESP_ERR_TIMEOUT
     */
    esp_err_t ui_feedback(ui_feedback_t type, const char *line1, const char *line2);

#ifdef __cplusplus
}
#endif

#endif
