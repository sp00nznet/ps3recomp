/*
 * ps3recomp - cellRtc HLE implementation
 *
 * Real-time clock using host system time.
 * PS3 epoch: Jan 1, 0001 00:00:00.000000 UTC
 * Tick unit: microseconds
 */

#include "cellRtc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <sys/timeb.h>
#else
#  include <sys/time.h>
#endif

/* Seconds between PS3 epoch (0001-01-01) and Unix epoch (1970-01-01) */
#define PS3_UNIX_EPOCH_DIFF  62135596800ULL

/* Microseconds per second */
#define USEC_PER_SEC  1000000ULL

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Get current Unix time in microseconds */
static u64 get_host_time_usec(void)
{
#ifdef _WIN32
    struct _timeb tb;
    _ftime_s(&tb);
    return (u64)tb.time * USEC_PER_SEC + (u64)tb.millitm * 1000ULL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * USEC_PER_SEC + (u64)tv.tv_usec;
#endif
}

/* Convert Unix time_t to PS3 tick */
static u64 time_t_to_tick(s64 t)
{
    return ((u64)t + PS3_UNIX_EPOCH_DIFF) * USEC_PER_SEC;
}

/* Convert PS3 tick to Unix time_t */
static s64 tick_to_time_t(u64 tick)
{
    return (s64)(tick / USEC_PER_SEC) - (s64)PS3_UNIX_EPOCH_DIFF;
}

/* Fill CellRtcDateTime from struct tm + microseconds */
static void tm_to_datetime(const struct tm* t, u32 usec, CellRtcDateTime* dt)
{
    dt->year        = (u16)(t->tm_year + 1900);
    dt->month       = (u16)(t->tm_mon + 1);
    dt->day         = (u16)t->tm_mday;
    dt->hour        = (u16)t->tm_hour;
    dt->minute      = (u16)t->tm_min;
    dt->second      = (u16)t->tm_sec;
    dt->microsecond = usec;
}

/* Convert CellRtcDateTime to struct tm */
static struct tm datetime_to_tm(const CellRtcDateTime* dt)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = dt->year - 1900;
    t.tm_mon   = dt->month - 1;
    t.tm_mday  = dt->day;
    t.tm_hour  = dt->hour;
    t.tm_min   = dt->minute;
    t.tm_sec   = dt->second;
    t.tm_isdst = -1;
    return t;
}

static int is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const int s_days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Day of week names for RFC formatting */
static const char* s_dow_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char* s_month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellRtcGetCurrentTick(CellRtcTick* pTick)
{
    if (!pTick)
        return CELL_EINVAL;

    u64 unix_usec = get_host_time_usec();
    pTick->tick = unix_usec + PS3_UNIX_EPOCH_DIFF * USEC_PER_SEC;

    return CELL_OK;
}

s32 cellRtcGetCurrentClockLocalTime(CellRtcDateTime* pClock)
{
    if (!pClock)
        return CELL_EINVAL;

    u64 host_usec = get_host_time_usec();
    time_t t = (time_t)(host_usec / USEC_PER_SEC);
    u32 usec = (u32)(host_usec % USEC_PER_SEC);

    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif

    tm_to_datetime(&local_tm, usec, pClock);
    return CELL_OK;
}

s32 cellRtcGetCurrentClockUtc(CellRtcDateTime* pClock)
{
    if (!pClock)
        return CELL_EINVAL;

    u64 host_usec = get_host_time_usec();
    time_t t = (time_t)(host_usec / USEC_PER_SEC);
    u32 usec = (u32)(host_usec % USEC_PER_SEC);

    struct tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &t);
#else
    gmtime_r(&t, &utc_tm);
#endif

    tm_to_datetime(&utc_tm, usec, pClock);
    return CELL_OK;
}

s32 cellRtcGetTime_t(const CellRtcTick* pTick, s64* pTime)
{
    if (!pTick || !pTime)
        return CELL_EINVAL;

    *pTime = tick_to_time_t(pTick->tick);
    return CELL_OK;
}

s32 cellRtcSetTime_t(CellRtcTick* pTick, s64 iTime)
{
    if (!pTick)
        return CELL_EINVAL;

    pTick->tick = time_t_to_tick(iTime);
    return CELL_OK;
}

s32 cellRtcTickAddYears(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    /* Convert tick to datetime, add years, convert back */
    CellRtcDateTime dt;
    cellRtcSetTick(&dt, pTick1);
    dt.year = (u16)((s32)dt.year + iAdd);

    /* Clamp Feb 29 -> Feb 28 if not leap year */
    if (dt.month == 2 && dt.day == 29 && !is_leap_year(dt.year))
        dt.day = 28;

    cellRtcGetTick(&dt, pTick0);
    return CELL_OK;
}

s32 cellRtcTickAddMonths(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    CellRtcDateTime dt;
    cellRtcSetTick(&dt, pTick1);

    s32 total_months = (s32)(dt.year - 1) * 12 + (s32)(dt.month - 1) + iAdd;
    if (total_months < 0) total_months = 0;

    dt.year  = (u16)(total_months / 12 + 1);
    dt.month = (u16)(total_months % 12 + 1);

    /* Clamp day */
    s32 max_day = cellRtcGetDaysInMonth((s32)dt.year, (s32)dt.month);
    if (dt.day > (u16)max_day)
        dt.day = (u16)max_day;

    cellRtcGetTick(&dt, pTick0);
    return CELL_OK;
}

s32 cellRtcTickAddDays(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    pTick0->tick = pTick1->tick + (s64)iAdd * 24LL * 3600LL * USEC_PER_SEC;
    return CELL_OK;
}

s32 cellRtcTickAddHours(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    pTick0->tick = pTick1->tick + (s64)iAdd * 3600LL * USEC_PER_SEC;
    return CELL_OK;
}

s32 cellRtcTickAddMinutes(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    pTick0->tick = pTick1->tick + (s64)iAdd * 60LL * USEC_PER_SEC;
    return CELL_OK;
}

s32 cellRtcTickAddSeconds(CellRtcTick* pTick0, const CellRtcTick* pTick1, s64 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    pTick0->tick = pTick1->tick + iAdd * (s64)USEC_PER_SEC;
    return CELL_OK;
}

s32 cellRtcTickAddMicroseconds(CellRtcTick* pTick0, const CellRtcTick* pTick1, s64 iAdd)
{
    if (!pTick0 || !pTick1)
        return CELL_EINVAL;

    pTick0->tick = pTick1->tick + iAdd;
    return CELL_OK;
}

s32 cellRtcConvertLocalTimeToUtc(const CellRtcTick* pLocalTime, CellRtcTick* pUtc)
{
    if (!pLocalTime || !pUtc)
        return CELL_EINVAL;

    /* Get local-to-UTC offset using the C runtime */
    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &now);
    gmtime_s(&utc_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
#endif

    time_t local_t = mktime(&local_tm);
    time_t utc_t   = mktime(&utc_tm);
    s64 offset_sec = (s64)(local_t - utc_t);

    pUtc->tick = pLocalTime->tick - (u64)(offset_sec * (s64)USEC_PER_SEC);
    return CELL_OK;
}

s32 cellRtcConvertUtcToLocalTime(const CellRtcTick* pUtc, CellRtcTick* pLocalTime)
{
    if (!pUtc || !pLocalTime)
        return CELL_EINVAL;

    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &now);
    gmtime_s(&utc_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
#endif

    time_t local_t = mktime(&local_tm);
    time_t utc_t   = mktime(&utc_tm);
    s64 offset_sec = (s64)(local_t - utc_t);

    pLocalTime->tick = pUtc->tick + (u64)(offset_sec * (s64)USEC_PER_SEC);
    return CELL_OK;
}

s32 cellRtcFormatRfc2822(char* pszDateTime, const CellRtcTick* pTick, s32 iTimeZoneMinutes)
{
    if (!pszDateTime || !pTick)
        return CELL_EINVAL;

    /* Convert tick to time_t, apply timezone offset */
    s64 unix_time = tick_to_time_t(pTick->tick);
    unix_time += (s64)iTimeZoneMinutes * 60;

    time_t t = (time_t)unix_time;
    struct tm tm_val;
#ifdef _WIN32
    gmtime_s(&tm_val, &t);
#else
    gmtime_r(&t, &tm_val);
#endif

    s32 tz_h = iTimeZoneMinutes / 60;
    s32 tz_m = abs(iTimeZoneMinutes) % 60;

    sprintf(pszDateTime, "%s, %02d %s %04d %02d:%02d:%02d %c%02d%02d",
            s_dow_names[tm_val.tm_wday],
            tm_val.tm_mday,
            s_month_names[tm_val.tm_mon],
            tm_val.tm_year + 1900,
            tm_val.tm_hour,
            tm_val.tm_min,
            tm_val.tm_sec,
            iTimeZoneMinutes >= 0 ? '+' : '-',
            abs(tz_h), tz_m);

    return CELL_OK;
}

s32 cellRtcFormatRfc3339(char* pszDateTime, const CellRtcTick* pTick, s32 iTimeZoneMinutes)
{
    if (!pszDateTime || !pTick)
        return CELL_EINVAL;

    s64 unix_time = tick_to_time_t(pTick->tick);
    unix_time += (s64)iTimeZoneMinutes * 60;

    time_t t = (time_t)unix_time;
    struct tm tm_val;
#ifdef _WIN32
    gmtime_s(&tm_val, &t);
#else
    gmtime_r(&t, &tm_val);
#endif

    if (iTimeZoneMinutes == 0) {
        sprintf(pszDateTime, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm_val.tm_year + 1900,
                tm_val.tm_mon + 1,
                tm_val.tm_mday,
                tm_val.tm_hour,
                tm_val.tm_min,
                tm_val.tm_sec);
    } else {
        s32 tz_h = iTimeZoneMinutes / 60;
        s32 tz_m = abs(iTimeZoneMinutes) % 60;
        sprintf(pszDateTime, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                tm_val.tm_year + 1900,
                tm_val.tm_mon + 1,
                tm_val.tm_mday,
                tm_val.tm_hour,
                tm_val.tm_min,
                tm_val.tm_sec,
                iTimeZoneMinutes >= 0 ? '+' : '-',
                abs(tz_h), tz_m);
    }

    return CELL_OK;
}

s32 cellRtcGetDayOfWeek(s32 year, s32 month, s32 day)
{
    /* Tomohiko Sakamoto's algorithm */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year;
    if (month < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[month - 1] + day) % 7;
}

s32 cellRtcGetDaysInMonth(s32 year, s32 month)
{
    if (month < 1 || month > 12)
        return 0;

    if (month == 2 && is_leap_year(year))
        return 29;

    return s_days_in_month[month - 1];
}

s32 cellRtcSetTick(CellRtcDateTime* pDateTime, const CellRtcTick* pTick)
{
    if (!pDateTime || !pTick)
        return CELL_EINVAL;

    u64 tick = pTick->tick;
    u32 usec = (u32)(tick % USEC_PER_SEC);
    s64 unix_time = tick_to_time_t(tick);

    time_t t = (time_t)unix_time;
    struct tm tm_val;
#ifdef _WIN32
    gmtime_s(&tm_val, &t);
#else
    gmtime_r(&t, &tm_val);
#endif

    tm_to_datetime(&tm_val, usec, pDateTime);
    return CELL_OK;
}

s32 cellRtcGetTick(const CellRtcDateTime* pDateTime, CellRtcTick* pTick)
{
    if (!pDateTime || !pTick)
        return CELL_EINVAL;

    struct tm t = datetime_to_tm(pDateTime);
#ifdef _WIN32
    time_t tt = _mkgmtime(&t);
#else
    time_t tt = timegm(&t);
#endif

    pTick->tick = time_t_to_tick((s64)tt) + pDateTime->microsecond;
    return CELL_OK;
}
