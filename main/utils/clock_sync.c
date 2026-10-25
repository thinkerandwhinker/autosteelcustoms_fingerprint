#include "clock_sync.h"

#include <stddef.h>
#include <stdlib.h>

#include "date_utils.h"
#include "ds3231.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"

static const char *TAG = "clock";

#define MIN_VALID_EPOCH_S 1735689600LL // 2025-01-01
#define RTC_WRITE_THRESHOLD_MS 2000    // when rtc should be rewrite?
#define RTC_REANCHOR_MS (10 * 60 * 1000)
#define RTC_REANCHOR_TOLERANCE_MS 1500 // below this the difference is just the RTC one second resolution

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_synced = false;
static int64_t s_offset_ms = 0;             // epoch = uptime + offset
static int64_t s_last_anchor_ms = 0;        // when last time ESP got set the time?
static volatile bool s_rtc_trusted = false; // the RTC holds the right time

static ds3231_handle_t s_rtc = NULL;
static SemaphoreHandle_t s_rtc_lock = NULL;

int64_t clock_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
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

    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > date_days_in_month(Y, M))
        return false;
    if (h > 23 || m > 59 || sec > 59)
        return false;

    int64_t days = days_from_civil(Y, M, D);
    *epoch_ms = (((days * 24 + h) * 60 + m) * 60 + sec) * 1000LL + ms;
    return true;
}

/**
 * epoch_ms is "now"
 */
static void anchor(int64_t epoch_ms)
{
    const int64_t up = clock_uptime_ms();
    portENTER_CRITICAL(&s_lock);
    s_offset_ms = epoch_ms - up;
    s_last_anchor_ms = up;
    s_synced = true;
    portEXIT_CRITICAL(&s_lock);
}

static int64_t rtc_centre_ms(const ds3231_reading_t *r) { return r->epoch_s * 1000 + 500; }

esp_err_t clock_sync_rtc_init(void)
{
    if (s_rtc != NULL)
        return ESP_OK;

    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK)
        return err;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL)
        return ESP_ERR_NO_MEM;

    ds3231_handle_t rtc = NULL;
    err = ds3231_init(i2c_bus_get(), &rtc);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "RTC DS3231 tidak ditemukan: %s", esp_err_to_name(err));
        vSemaphoreDelete(lock);
        return err;
    }
    s_rtc_lock = lock;
    s_rtc = rtc;

    ds3231_reading_t r;
    err = ds3231_read(s_rtc, &r);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "RTC tidak bisa dibaca: %s", esp_err_to_name(err));
        return err; // stays attached
    }
    if (r.oscillator_stopped || r.epoch_s < MIN_VALID_EPOCH_S)
    {
        ESP_LOGW(TAG, "RTC belum valid (osilator berhenti atau belum pernah diset), menunggu sinkronisasi server");
        return ESP_OK;
    }

    anchor(rtc_centre_ms(&r));
    s_rtc_trusted = true;
    ESP_LOGI(TAG, "Waktu dari RTC: %lld", (long long)r.epoch_s);
    return ESP_OK;
}

/**
 * automatic following time server when needed
 */
static void rtc_follow_server(int64_t server_epoch_ms)
{
    if (s_rtc == NULL || server_epoch_ms < MIN_VALID_EPOCH_S * 1000)
        return;

    xSemaphoreTake(s_rtc_lock, portMAX_DELAY);
    ds3231_reading_t r;
    bool agree = false;
    if (ds3231_read(s_rtc, &r) == ESP_OK && !r.oscillator_stopped)
        agree = llabs(server_epoch_ms - rtc_centre_ms(&r)) < RTC_WRITE_THRESHOLD_MS;

    if (agree)
    {
        s_rtc_trusted = true;
    }
    else if (ds3231_set_epoch(s_rtc, (server_epoch_ms + 500) / 1000) == ESP_OK) // error <= 0.5 s
    {
        s_rtc_trusted = true;
        ESP_LOGI(TAG, "RTC disetel dari server");
    }
    else
    {
        ESP_LOGW(TAG, "Gagal menyetel RTC");
    }
    xSemaphoreGive(s_rtc_lock);
}

/**
 * re-set time on RTC
 */
static void rtc_reanchor_if_stale(void)
{
    if (s_rtc == NULL)
        return;
    const int64_t up = clock_uptime_ms();
    portENTER_CRITICAL(&s_lock);
    const bool due = s_rtc_trusted && (up - s_last_anchor_ms) >= RTC_REANCHOR_MS;
    if (due)
        s_last_anchor_ms = up; // also when the read fails below
    const int64_t estimate = up + s_offset_ms;
    portEXIT_CRITICAL(&s_lock);
    if (!due)
        return;

    xSemaphoreTake(s_rtc_lock, portMAX_DELAY);
    ds3231_reading_t r;
    const esp_err_t err = ds3231_read(s_rtc, &r);
    xSemaphoreGive(s_rtc_lock);

    if (err != ESP_OK || r.oscillator_stopped)
    {
        ESP_LOGW(TAG, "RTC tidak bisa dipakai untuk koreksi: %s", err != ESP_OK ? esp_err_to_name(err) : "osilator berhenti");
        if (err == ESP_OK)
            s_rtc_trusted = false;
        return;
    }
    if (llabs(estimate - rtc_centre_ms(&r)) > RTC_REANCHOR_TOLERANCE_MS)
    {
        ESP_LOGI(TAG, "Jam dikoreksi dari RTC (selisih %lld ms)", (long long)(rtc_centre_ms(&r) - estimate));
        anchor(rtc_centre_ms(&r));
    }
}

void clock_sync_set_epoch_ms(int64_t server_epoch_ms)
{
    anchor(server_epoch_ms);
    rtc_follow_server(server_epoch_ms);
}

bool clock_sync_now_ms(int64_t *out)
{
    rtc_reanchor_if_stale();

    portENTER_CRITICAL(&s_lock);
    bool synced = s_synced;
    int64_t off = s_offset_ms;
    portEXIT_CRITICAL(&s_lock);
    if (!synced)
        return false;
    *out = clock_uptime_ms() + off;
    return true;
}

clock_source_t clock_sync_source(void)
{
    portENTER_CRITICAL(&s_lock);
    const clock_source_t src = !s_synced ? CLOCK_SOURCE_NONE : (s_rtc_trusted ? CLOCK_SOURCE_RTC : CLOCK_SOURCE_SERVER);
    portEXIT_CRITICAL(&s_lock);
    return src;
}

const char *clock_source_str(clock_source_t src)
{
    switch (src)
    {
    case CLOCK_SOURCE_RTC:
        return "RTC";
    case CLOCK_SOURCE_SERVER:
        return "SERVER";
    default:
        return "UNTRUSTED";
    }
}