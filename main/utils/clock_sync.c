#include "clock_sync.h"

#include <stddef.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_synced = false;
static int64_t s_offset_ms = 0; // epoch = uptime + offset

int64_t clock_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    // days in month variant
    static const int d[] = {
        31,
        28,
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31,
    };

    return (m == 2 && is_leap(y) ? 29 : d[m - 1]);
}

// days since 1970-01-01
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);

    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

static bool read_digits(const char **p, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++)
    {
        char c = (*p)[i];
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    *p += n;
    *out = v;
    return true;
}

bool clock_parse_iso8601_utc(const char *s, int64_t *epoch_ms)
{
    if (s == NULL)
        return false;
    const char *p = s;
    int Y, M, D, h, m, sec;

    if (!read_digits(&p, 4, &Y) || *p++ != '-')
        return false;
    if (!read_digits(&p, 2, &M) || *p++ != '-')
        return false;
    if (!read_digits(&p, 2, &D) || *p++ != 'T')
        return false;
    if (!read_digits(&p, 2, &h) || *p++ != ':')
        return false;
    if (!read_digits(&p, 2, &m) || *p++ != ':')
        return false;
    if (!read_digits(&p, 2, &sec))
        return false;

    int ms = 0;
    if (*p == '.')
    {
        p++;
        int digits = 0, mult = 100;
        while (*p >= '0' && *p <= '9')
        {
            if (digits < 3)
            {
                ms += (*p - '0') * mult;
                mult /= 10;
            }
            digits++;
            p++;
        }
        if (digits == 0)
            return false;
    }
    if (*p++ != 'Z' || *p != '\0')
        return false;

    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > days_in_month(Y, M))
        return false;
    if (h > 23 || m > 59 || sec > 59)
        return false;

    int64_t days = days_from_civil(Y, M, D);
    *epoch_ms = (((days * 24 + h) * 60 + m) * 60 + sec) * 1000LL + ms;
    return true;
}

void clock_sync_set_epoch_ms(int64_t server_epoch_ms)
{
    int64_t off = server_epoch_ms - clock_uptime_ms();
    portENTER_CRITICAL(&s_lock);
    s_offset_ms = off;
    s_synced = true;
    portEXIT_CRITICAL(&s_lock);
}

bool clock_sync_now_ms(int64_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool synced = s_synced;
    int64_t off = s_offset_ms;
    portEXIT_CRITICAL(&s_lock);
    if (!synced)
        return false;
    *out = clock_uptime_ms() + off;
    return true;
}