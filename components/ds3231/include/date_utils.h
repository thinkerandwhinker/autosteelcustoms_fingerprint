/**
 * DATE_UTILS_H
 *
 * core calendar math utilities, trusted algorithm source :)
 * conversion civil calendar (since 1970-01-01)
 *
 * range: arbitrary year ranges (negative years supported)
 * resolution: day / month
 */

#ifndef DATE_UTILS_H
#define DATE_UTILS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * check if year is leap year
     */
    static inline bool date_is_leap(int y)
    {
        return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    }

    /**
     * get days count in a given month
     *
     * 1 = January
     * 2 = February
     * ...
     * 12 = December
     */
    static inline int date_days_in_month(int y, int m)
    {
        static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return (m == 2 && date_is_leap(y)) ? 29 : d[m - 1];
    }

    /**
     * count day since 1970-01-01
     */
    static inline int64_t days_from_civil(int y, int m, int d)
    {
        y -= (m <= 2);
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int64_t yoe = y - era * 400;
        const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    /**
     * reverse from function above
     */
    static inline void civil_from_days(int64_t z, int *y, int *m, int *d)
    {
        z += 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const int64_t doe = z - era * 146097;
        const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const int64_t mp = (5 * doy + 2) / 153;
        *d = (int)(doy - (153 * mp + 2) / 5 + 1);
        *m = (int)(mp < 10 ? mp + 3 : mp - 9);
        *y = (int)(yoe + era * 400 + (*m <= 2));
    }

#ifdef __cplusplus
}
#endif

#endif