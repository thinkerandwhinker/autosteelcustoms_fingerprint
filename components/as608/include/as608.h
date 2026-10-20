/**
 * AS608_H
 *
 * driver for AS608/FPM10A optical fingerprint module, via UART
 *
 * this component only talk to the sensor. NO MQTT, NO NVS, NO UI inside.
 * every function return as608_status_t, caller (fpm_task) decide what to do next
 * and map status into the backend contract (as608_status_str() give UPPER_SNAKE name)
 *
 * threading:
 * - all public function (except as608_init() and as608_cancel()) is serialized by an internal mutex,
 *   but it's designed to be called from ONE task (fpm_task)
 * - as608_enroll_step() BLOCKING until step finished or timeout, never call it from mqtt_task
 *
 * enrollment flow (one finger = 3 steps + save):
 *
 *   PLACE_FINGER  -> capture #1, make template buffer 1, search library (duplicate check)
 *   REMOVE_FINGER -> wait until sensor free (finger removed)
 *   PLACE_AGAIN   -> capture #2, make template buffer 2, merge both into one model
 *   as608_save_template(id) -> write model into sensor flash at slot id
 *
 * slot id (fingerId) start from 0, valid range is 0 .. capacity-1 (capacity read from sensor on init)
 */
#ifndef AS608_H
#define AS608_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        AS608_OK = 0,
        AS608_ERR_INVALID_ARG, // given parameter wrong
        AS608_ERR_BAD_STATE,   // not initialized, stepnya ga sesuai urutan, ato gada template model yang disimpen
        AS608_ERR_INTERNAL,    // failed to setup driver
        AS608_ERR_NO_RESPONSE, // sensor no respon/rusak/wiring problem
        AS608_ERR_COMM,        // garbled reply: bad header, address, or checksum
        AS608_ERR_TIMEOUT,
        AS608_ERR_CANCELLED,      // as608_cancel() was called
        AS608_ERR_NO_FINGER,      // finger didn't place on sensor
        AS608_ERR_IMAGE_FAIL,     // sensor failed to capture the finger "image"
        AS608_ERR_POOR_IMAGE,     // image too bad or burik to read by sensor
        AS608_ERR_MISMATCH,       // 2nd finger is not (hampir) same with 1st finger
        AS608_ERR_DUPLICATE,      // finger already exist
        AS608_ERR_NOT_FOUND,      // findOne/findAll returning undefined/no data
        AS608_ERR_BAD_ID,         // fingerId outside of sensor library
        AS608_ERR_SLOT_OCCUPIED,  // slot already hold a template
        AS608_ERR_FLASH,          // sensor failed to write its flash
        AS608_ERR_ERASE_FAILED,   // sensor refused to delete / empty (0x10 / 0x11)
        AS608_ERR_WRONG_PASSWORD, // sensor password mismatch
        AS608_ERR_SENSOR,         // any other non-zero confirm code, see as608_last_confirm_code()
    } as608_status_t;

    typedef enum
    {
        AS608_STEP_PLACE_FINGER = 0,
        AS608_STEP_REMOVE_FINGER,
        AS608_STEP_PLACE_AGAIN,
    } as608_enroll_step_t;

    typedef struct
    {
        uart_port_t uart_num;
        gpio_num_t tx_pin;   // ESP TX => sensor RX
        gpio_num_t rx_pin;   // ESP RX => sensor TX
        gpio_num_t wake_pin; // WAK / touch output (baru rencana aja)
        uint32_t baud;       // sensor default is 57600
        uint32_t address;    // sensor default is 0xFFFFFFFF
        uint32_t password;   // sensor default is 0x00000000
    } as608_config_t;

/**
 * default config, UART2 + factory sensor settings
 * usage: as608_config_t cfg = AS608_CONFIG_DEFAULT(PIN_FPM_UART_TX, PIN_FPM_UART_RX, PIN_FPM_WAKE);
 */
#define AS608_CONFIG_DEFAULT(tx, rx, wake) \
    {                                      \
        .uart_num = UART_NUM_2,            \
        .tx_pin = (gpio_num_t)(tx),        \
        .rx_pin = (gpio_num_t)(rx),        \
        .wake_pin = (gpio_num_t)(wake),    \
        .baud = 57600,                     \
        .address = 0xFFFFFFFF,             \
        .password = 0x00000000,            \
    }

    typedef struct
    {
        uint16_t status_reg;
        uint16_t system_id;
        uint16_t capacity;       // total slots
        uint16_t security_level; // 1 to 5
        uint32_t address;
        uint16_t packet_size; // 32byte, 64byte, 128byte, 256byte
        uint32_t baud;
    } as608_info_t;

    /**
     * configure UART, verify sensor password, read slots capacity
     *
     * call once at boot, not thread safe. idempotent: return AS608_OK when already initialized
     * failure is not fatal for the firmware, the caller may keep running without sensor
     */
    as608_status_t as608_init(const as608_config_t *cfg);

    bool as608_is_ready(void);

    as608_status_t as608_get_info(as608_info_t *out);

    /**
     * run ONE enrollment step, blocking up to timeout_ms for the user (finger place / remove)
     *
     * PLACE_AGAIN needs a successful PLACE_FINGER and then REMOVE_FINGER before it, otherwise AS608_ERR_BAD_STATE
     * PLACE_FINGER always restarts the flow
     *
     * PLACE_FINGER  : OK, TIMEOUT, POOR_IMAGE, IMAGE_FAIL, CANCELLED,
     *                 DUPLICATE (finger already enrolled, *duplicate_id = slot that holds it)
     *                 after POOR_IMAGE / DUPLICATE the finger is still on sensor, call REMOVE_FINGER
     *                 before trying again, or it will be captured again immediately
     * REMOVE_FINGER : OK, TIMEOUT, CANCELLED. allowed at any time, can be repeated after TIMEOUT
     * PLACE_AGAIN   : OK (model ready for as608_save_template), TIMEOUT, POOR_IMAGE, IMAGE_FAIL,
     *                 MISMATCH, CANCELLED
     *                 after POOR_IMAGE / MISMATCH finger is still on sensor, call REMOVE_FINGER then
     *                 PLACE_AGAIN again (1st capture is kept)
     *
     * @param duplicate_id can be NULL, only written when AS608_ERR_DUPLICATE
     */
    as608_status_t as608_enroll_step(as608_enroll_step_t step, uint32_t timeout_ms, uint16_t *duplicate_id);

    /**
     * write the model made by enrollment into sensor flash at slot finger_id
     * refuse to overwrite: AS608_ERR_SLOT_OCCUPIED when the slot already has a template
     * model is consumed on success (one enrollment = one save)
     */
    as608_status_t as608_save_template(uint16_t finger_id);

    /**
     * delete exactly one slot (DeleteChar N=1)
     * does not tell if slot was occupied, prove it with as608_check_slot_empty()
     */
    as608_status_t as608_delete_template(uint16_t finger_id);

    /**
     * wipe the whole fingerprint library (Empty / 0x0D)
     * prove it with as608_template_count()
     */
    as608_status_t as608_empty_database(void);

    /**
     * verification, using the sensor index table (ReadIndexTable / 0x1F)
     *
     * AS608_OK            => PROVEN empty
     * AS608_ERR_SLOT_OCCUPIED => slot holds a template
     * anything else       => can't tell, treat as not verified
     */
    as608_status_t as608_check_slot_empty(uint16_t finger_id);

    /**
     * how many templates stored in sensor library (TemplateNum / 0x1D)
     */
    as608_status_t as608_template_count(uint16_t *count);

    /**
     * interrupt the step that is waiting for finger, safe to call from any task
     * sticky: if no step is waiting, the next REMOVE_FINGER / PLACE_AGAIN returns CANCELLED
     * (flag is cleared when consumed, or when PLACE_FINGER starts a fresh flow)
     */
    void as608_cancel(void);

    /**
     * raw confirm code of the last sensor reply (0x00 = ok), for logging
     */
    uint8_t as608_last_confirm_code(void);

    /**
     * UPPER_SNAKE name, e.g. "POOR_IMAGE", never NULL
     */
    const char *as608_status_str(as608_status_t status);

#ifdef __cplusplus
}
#endif

#endif