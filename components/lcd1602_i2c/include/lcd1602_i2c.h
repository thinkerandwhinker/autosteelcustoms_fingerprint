/**
 * LCD1602_I2C_H
 *
 * lCD 16x2 driver (HD44780), with I2C PCF8574 installed on 4 bit mode
 * I2C expander mapping: P0=RS, P1=RW, P2=EN, P3=Backlight. Bolongan lainnya urutan
 * driver: I2C master (ESP-IDF >= 5.1).
 */
#ifndef LCD1602_I2C_H
#define LCD1602_I2C_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LCD1602_COLS 16
#define LCD1602_ROWS 2

    typedef struct lcd1602 *lcd1602_handle_t;

    /**
     * add lcd into bus
     * run ordered initialization
     */
    esp_err_t lcd1602_init(i2c_master_bus_handle_t bus, uint8_t addr, lcd1602_handle_t *out);

    void lcd1602_deinit(lcd1602_handle_t lcd);

    esp_err_t lcd1602_clear(lcd1602_handle_t lcd);

    /**
     * write for full one line
     * do truncate or pad with space to 16 chars
     */
    esp_err_t lcd1602_print_line(lcd1602_handle_t lcd, uint8_t row, const char *text);

    esp_err_t lcd1602_set_backlight(lcd1602_handle_t lcd, bool on);

#ifdef __cplusplus
}
#endif

#endif
