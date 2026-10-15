#include "lcd1602_i2c.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd1602";

#define PIN_RS 0x01
#define PIN_EN 0x04
#define PIN_BL 0x08

#define CMD_CLEAR 0x01
#define CMD_ENTRY_MODE 0x06 // no shift, increment
#define CMD_DISPLAY_OFF 0x08
#define CMD_DISPLAY_ON 0x0C // display on, cursor off, blink off
#define CMD_FUNC_4BIT_2LINE 0x28
#define CMD_SET_DDRAM 0x80

#define I2C_SPEED_HZ 100000
#define I2C_TIMEOUT_MS 100
#define MAX_XFER (4 + LCD1602_COLS * 4) // cursor + 16 char

struct lcd1602
{
    i2c_master_dev_handle_t dev;
    bool backlight;
};

/**
 * I2C take aboud 90 µs every 100kHz (Kata nxp semiconductor)
 * so, width of EN pulse and basic command execution will not delay
 * harusnya sih gitu ya...
 */
static esp_err_t expander_write(lcd1602_handle_t lcd, const uint8_t *buf, size_t len)
{
    uint8_t tmp[MAX_XFER];
    if (len > sizeof(tmp))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t bl = lcd->backlight ? PIN_BL : 0;
    for (size_t i = 0; i < len; i++)
    {
        tmp[i] = buf[i] | bl;
    }
    return i2c_master_transmit(lcd->dev, tmp, len, I2C_TIMEOUT_MS);
}

static esp_err_t write_nibble(lcd1602_handle_t lcd, uint8_t nibble)
{
    uint8_t d = (uint8_t)((nibble & 0x0F) << 4);
    uint8_t buf[2] = {(uint8_t)(d | PIN_EN), d};
    return expander_write(lcd, buf, 2);
}

/**
 * fill 4 bytes: (hi|EN, hi, lo|EN, lo) to buffer
 * return 4
 */
static size_t encode_byte(uint8_t *buf, uint8_t value, bool rs)
{
    uint8_t r = rs ? PIN_RS : 0;
    uint8_t hi = (uint8_t)(value & 0xF0);
    uint8_t lo = (uint8_t)((value << 4) & 0xF0);
    buf[0] = (uint8_t)(hi | r | PIN_EN);
    buf[1] = (uint8_t)(hi | r);
    buf[2] = (uint8_t)(lo | r | PIN_EN);
    buf[3] = (uint8_t)(lo | r);
    return 4;
}

static esp_err_t write_cmd(lcd1602_handle_t lcd, uint8_t cmd)
{
    uint8_t buf[4];
    encode_byte(buf, cmd, false);
    return expander_write(lcd, buf, 4);
}

esp_err_t lcd1602_init(i2c_master_bus_handle_t bus, uint8_t addr, lcd1602_handle_t *out)
{
    if (bus == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lcd1602_handle_t lcd = calloc(1, sizeof(*lcd));
    if (lcd == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lcd->backlight = true;

    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &lcd->dev);
    if (err != ESP_OK)
    {
        free(lcd);
        return err;
    }

#define STEP(expr)         \
    do                     \
    {                      \
        err = (expr);      \
        if (err != ESP_OK) \
            goto fail;     \
    } while (0)

    vTaskDelay(pdMS_TO_TICKS(50)); // add some delay while waiting for the LCD

    /**
     * reset order to 4 bit mode display
     * (based on HD44780 datasheet)
     */
    STEP(write_nibble(lcd, 0x03));
    esp_rom_delay_us(4500);
    STEP(write_nibble(lcd, 0x03));
    esp_rom_delay_us(4500);
    STEP(write_nibble(lcd, 0x03));
    esp_rom_delay_us(150);
    STEP(write_nibble(lcd, 0x02));
    esp_rom_delay_us(150);

    STEP(write_cmd(lcd, CMD_FUNC_4BIT_2LINE));
    STEP(write_cmd(lcd, CMD_DISPLAY_OFF));
    STEP(write_cmd(lcd, CMD_CLEAR));
    esp_rom_delay_us(2000);
    STEP(write_cmd(lcd, CMD_ENTRY_MODE));
    STEP(write_cmd(lcd, CMD_DISPLAY_ON));
#undef STEP

    ESP_LOGI(TAG, "LCD siap di alamat 0x%02X", addr);
    *out = lcd;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Inisialisasi LCD gagal: %s", esp_err_to_name(err));
    i2c_master_bus_rm_device(lcd->dev);
    free(lcd);
    return err;
}

void lcd1602_deinit(lcd1602_handle_t lcd)
{
    if (lcd == NULL)
    {
        return;
    }
    i2c_master_bus_rm_device(lcd->dev);
    free(lcd);
}

esp_err_t lcd1602_clear(lcd1602_handle_t lcd)
{
    esp_err_t err = write_cmd(lcd, CMD_CLEAR);
    esp_rom_delay_us(2000);
    return err;
}

esp_err_t lcd1602_print_line(lcd1602_handle_t lcd, uint8_t row, const char *text)
{
    if (lcd == NULL || row >= LCD1602_ROWS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[MAX_XFER];
    size_t n = encode_byte(buf, (uint8_t)(CMD_SET_DDRAM | (row ? 0x40 : 0x00)), false);

    size_t len = text ? strlen(text) : 0;
    for (size_t i = 0; i < LCD1602_COLS; i++)
    {
        char c = (i < len) ? text[i] : ' ';
        if (c < 0x20 || c > 0x7E)
        {
            c = '?';
        }
        n += encode_byte(buf + n, (uint8_t)c, true);
    }
    return expander_write(lcd, buf, n); // 1 I2C transaction/line
}

esp_err_t lcd1602_set_backlight(lcd1602_handle_t lcd, bool on)
{
    lcd->backlight = on;
    uint8_t zero = 0;
    return expander_write(lcd, &zero, 1);
}
