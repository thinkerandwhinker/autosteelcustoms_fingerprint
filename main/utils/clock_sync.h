/**
 * CLOCK_SYNC_H
 *
 * sync server time with ESP
 */

#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

#include <stdbool.h>
#include <stdint.h>

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

    /**
     * sync server time
     */
    void clock_sync_set_epoch_ms(int64_t server_epoch_ms);

    /**
     * check latest time sync
     */
    bool clock_sync_now_ms(int64_t *out);

#ifdef __cplusplus
}
#endif

#endif