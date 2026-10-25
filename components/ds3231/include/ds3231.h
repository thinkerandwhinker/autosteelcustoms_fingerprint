/**
 * DS3231_H
 *
 * driver for DS3231 battery backed RTC (I2C, address 0x68)
 * curr battery type: CR3032 3V non-rechargeable
 *
 * rage: 2000-01-01T00:00:00.000Z .. 2099-12-31T23:59:59.999Z (UTC only)
 * resolution: second
 * bus: shared bus with lcd 16x2
 *
 * potentiol problems (because using shared I2C bus):
 * I2C driver biasanya lock untuk satu proses/satu transaksi. Jadi, salah satunya biasanya kalah.
 * Solusi sementara ya pilih siapa prioritasnya, RTC di kalahin karena ga selalu bakal dipake.
 */

#ifndef DS3231_H
#define DS3231_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DS3231_I2C_ADDR 0x68
#define DS3231_EPOCH_MIN 946684800LL  // start range 2000-01-01
#define DS3231_EPOCH_MAX 4102444799LL // end range 2099-12-31

    typedef struct ds3231_dev *ds3231_handle_t;
    typedef struct
    {
        int64_t epoch_s;
        bool oscillator_stopped; // flag: is module died? is module not configure? dkk
    } ds3231_reading_t;

    /**
     * module prepare
     *
     * throw failed when module not responding, maybe caused by battery dead. maybe loh ya...
     */
    esp_err_t ds3231_init(i2c_master_bus_handle_t bus, ds3231_handle_t *out);

    /**
     * get time and validity of time
     *
     * throw failed when no time got from backend/time not set yet
     */
    esp_err_t ds3231_read(ds3231_handle_t h, ds3231_reading_t *out);

    /**
     * to set time/sync time
     */
    esp_err_t ds3231_set_epoch(ds3231_handle_t h, int64_t epoch);

    /**
     * init juga, tapi buat task
     */
    void ds3231_deinit(ds3231_handle_t h);

#ifdef __cplusplus
}

#endif

#endif