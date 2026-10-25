/**
 * CLOCK_SYNC_H
 *
 * the one place that knows what time it is
 *
 * time comes from two places:
 * - the server: PONG gives the time every ~30 s, accurate to a second or so, but only while online
 * - the DS3231 RTC (optional): battery backed, so the time is right from boot and while offline
 *
 * RTC is the basisnya, server time is only refference
 *
 * rules:
 * - kalo waktunya memang udah valid di RTC, ga perlu set ulang waktu boot
 * - PONG: kalau pas PONG dateng terus ternyata selisih 2 detik dari waktu server, harus di set ulang
 *
 * accuracy is about +-2 s, scans are timestamped to the second
 *
 * notes:
 * kalo "clock_sync_now_ms()" return false, it means "i don't know either"
 */

#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * uptime counter
     */
    int64_t clock_uptime_ms(void);

    /**
     * iso date-time validator
     *
     * true if format: YYYY-MM-DDTHH:MM:SS.sssZ
     */
    bool clock_parse_iso8601_utc(const char *s, int64_t *epoch_ms);

    typedef enum
    {
        CLOCK_SOURCE_NONE = 0, // never synced
        CLOCK_SOURCE_SERVER,
        CLOCK_SOURCE_RTC,
    } clock_source_t;

    /**
     * untuk boot
     */
    esp_err_t clock_sync_rtc_init(void);

    /**
     * sync server time
     */
    void clock_sync_set_epoch_ms(int64_t server_epoch_ms);

    /**
     * check latest time sync
     */
    bool clock_sync_now_ms(int64_t *out);

    /**
     * where the time comes from?
     */
    clock_source_t clock_sync_source(void);

    /**
     * "RTC" | "SERVER" | "UNTRUSTED"
     */
    const char *clock_source_str(clock_source_t src);

#ifdef __cplusplus
}
#endif

#endif