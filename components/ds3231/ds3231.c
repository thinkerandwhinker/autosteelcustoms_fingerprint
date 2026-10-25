#include "ds3231.h"

#include <stdlib.h>
#include <string.h>

#include "date_utils.h"

#include "esp_log.h"

static const char *TAG = "ds3231";

#define I2C_SPEED_HZ 10000
#define I2C_TIMEOUT_MS 100

/* registers */
#define REG_SECONDS 0x00 // 0x00..0x06: seconds, minutes, hours, weekday, date, month, year
#define REG_CONTROL 0x0E
#define REG_STATUS 0x0F
#define REG_COUNT 16

#define CTRL_EOSC 0x80
#define STATUS_OSF 0x80
#define HOURS_12H 0x40
#define HOURS_PM 0x20
#define MONTH_CENTURY 0x80

struct ds3231_dev
{
    i2c_master_dev_handle_t dev;
};

/**
 * -1 when param not valid number
 */
static int bcd_to_int(uint8_t v)
{
    const int hi = v >> 4, lo = v & 0x0F;
    return (hi > 9 || lo > 9) ? -1 : hi * 10 + lo;
}

static uint8_t int_to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/**
 * bus
 */
static esp_err_t read_regs(ds3231_handle_t h, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(h->dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

static esp_err_t write_regs(ds3231_handle_t h, uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t tmp[9]; // register address
    if (n > sizeof(tmp) - 1)
        return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(&tmp[1], data, n);
    return i2c_master_transmit(h->dev, tmp, n + 1, I2C_TIMEOUT_MS);
}

/**
 * throw err when r is not valid date time
 */
static esp_err_t decode_time(const uint8_t *r, int64_t *epoch_s)
{
    const int sec = bcd_to_int(r[0] & 0x7F);
    const int min = bcd_to_int(r[1] & 0x7F);
    const int date = bcd_to_int(r[4] & 0x3F);
    const int month = bcd_to_int(r[5] & 0x1F);
    const int yy = bcd_to_int(r[6]);
    int hour;

    if (r[2] & HOURS_12H)
    {
        const int h12 = bcd_to_int(r[2] & 0x1F);
        if (h12 < 1 || h12 > 12)
            return ESP_ERR_INVALID_RESPONSE;
        hour = (h12 % 12) + ((r[2] & HOURS_PM) ? 12 : 0);
    }
    else
    {
        hour = bcd_to_int(r[2] & 0x3F);
    }

    const int year = ((r[5] & MONTH_CENTURY) ? 2100 : 2000) + yy;
    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23 || yy < 0 || month < 1 || month > 12 ||
        date < 1 || date > date_days_in_month(year, month))
        return ESP_ERR_INVALID_RESPONSE;

    *epoch_s = ((days_from_civil(year, month, date) * 24 + hour) * 60 + min) * 60 + sec;
    return ESP_OK;
}

/**
 * public
 */
esp_err_t ds3231_init(i2c_master_bus_handle_t bus, ds3231_handle_t *out)
{
    if (bus == NULL || out == NULL)
        return ESP_ERR_INVALID_ARG;

    struct ds3231_dev *h = calloc(1, sizeof(*h));
    if (h == NULL)
        return ESP_ERR_NO_MEM;

    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &h->dev);
    if (err != ESP_OK)
    {
        free(h);
        return err;
    }

    // does it answer?
    uint8_t ctrl;
    err = read_regs(h, REG_CONTROL, &ctrl, 1);
    if (err == ESP_OK && (ctrl & CTRL_EOSC))
    {
        ctrl &= (uint8_t)~CTRL_EOSC;
        err = write_regs(h, REG_CONTROL, &ctrl, 1); // set to battery mod
        ESP_LOGW(TAG, "EOSC aktif, osilator dinyalakan untuk mode baterai");
    }
    if (err != ESP_OK)
    {
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return err;
    }

    *out = h;
    return ESP_OK;
}

esp_err_t ds3231_read(ds3231_handle_t h, ds3231_reading_t *out)
{
    if (h == NULL || out == NULL)
        return ESP_ERR_INVALID_ARG;

    uint8_t r[REG_COUNT];
    esp_err_t err = read_regs(h, REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK)
        return err;

    err = decode_time(r, &out->epoch_s);
    if (err != ESP_OK)
        return err;
    out->oscillator_stopped = (r[REG_STATUS] & STATUS_OSF) != 0;
    return ESP_OK;
}

esp_err_t ds3231_set_epoch(ds3231_handle_t h, int64_t epoch_s)
{
    if (h == NULL)
        return ESP_ERR_INVALID_ARG;
    if (epoch_s < DS3231_EPOCH_MIN || epoch_s > DS3231_EPOCH_MAX)
        return ESP_ERR_INVALID_ARG;

    const int64_t days = epoch_s / 86400;
    const int rem = (int)(epoch_s % 86400);
    int y, m, d;
    civil_from_days(days, &y, &m, &d);

    const uint8_t regs[7] = {
        int_to_bcd(rem % 60),
        int_to_bcd((rem / 60) % 60),
        int_to_bcd(rem / 3600),
        (uint8_t)(1 + (days + 4) % 7),
        int_to_bcd(d),
        int_to_bcd(m), // 2000 to 2099
        int_to_bcd(y - 2000),
    };
    esp_err_t err = write_regs(h, REG_SECONDS, regs, sizeof(regs));
    if (err != ESP_OK)
        return err; // can't trus the ESP time source

    uint8_t status;
    err = read_regs(h, REG_STATUS, &status, 1);
    if (err == ESP_OK && (status & STATUS_OSF))
    {
        status &= (uint8_t)~STATUS_OSF;
        err = write_regs(h, REG_STATUS, &status, 1);
    }
    return err;
}

void ds3231_deinit(ds3231_handle_t h)
{
    if (h == NULL)
        return;
    i2c_master_bus_rm_device(h->dev);
    free(h);
}
