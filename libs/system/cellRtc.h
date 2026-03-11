/*
 * ps3recomp - cellRtc HLE
 *
 * Real-time clock: ticks, date/time conversion, formatting.
 */

#ifndef PS3RECOMP_CELL_RTC_H
#define PS3RECOMP_CELL_RTC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/*
 * PS3 epoch: Jan 1, 0001 00:00:00.000000 UTC
 * Offset from Unix epoch (Jan 1, 1970) = 62135596800 seconds
 * Ticks are in microseconds.
 */
#define CELL_RTC_UNIX_EPOCH_OFFSET  62135596800ULL

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellRtcTick {
    u64 tick;  /* microseconds since PS3 epoch (Jan 1, 0001) */
} CellRtcTick;

typedef struct CellRtcDateTime {
    u16 year;
    u16 month;     /* 1-12 */
    u16 day;       /* 1-31 */
    u16 hour;      /* 0-23 */
    u16 minute;    /* 0-59 */
    u16 second;    /* 0-59 */
    u32 microsecond; /* 0-999999 */
} CellRtcDateTime;

/* Alias used by some SDK headers */
typedef CellRtcDateTime CellRtcTime;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellRtcGetCurrentTick(CellRtcTick* pTick);

s32 cellRtcGetCurrentClockLocalTime(CellRtcDateTime* pClock);

s32 cellRtcGetCurrentClockUtc(CellRtcDateTime* pClock);

s32 cellRtcGetTime_t(const CellRtcTick* pTick, s64* pTime);

s32 cellRtcSetTime_t(CellRtcTick* pTick, s64 iTime);

s32 cellRtcTickAddYears(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd);
s32 cellRtcTickAddMonths(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd);
s32 cellRtcTickAddDays(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd);
s32 cellRtcTickAddHours(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd);
s32 cellRtcTickAddMinutes(CellRtcTick* pTick0, const CellRtcTick* pTick1, s32 iAdd);
s32 cellRtcTickAddSeconds(CellRtcTick* pTick0, const CellRtcTick* pTick1, s64 iAdd);
s32 cellRtcTickAddMicroseconds(CellRtcTick* pTick0, const CellRtcTick* pTick1, s64 iAdd);

s32 cellRtcConvertLocalTimeToUtc(const CellRtcTick* pLocalTime, CellRtcTick* pUtc);
s32 cellRtcConvertUtcToLocalTime(const CellRtcTick* pUtc, CellRtcTick* pLocalTime);

s32 cellRtcFormatRfc2822(char* pszDateTime, const CellRtcTick* pTick, s32 iTimeZoneMinutes);
s32 cellRtcFormatRfc3339(char* pszDateTime, const CellRtcTick* pTick, s32 iTimeZoneMinutes);

s32 cellRtcGetDayOfWeek(s32 year, s32 month, s32 day);
s32 cellRtcGetDaysInMonth(s32 year, s32 month);

s32 cellRtcSetTick(CellRtcDateTime* pDateTime, const CellRtcTick* pTick);
s32 cellRtcGetTick(const CellRtcDateTime* pDateTime, CellRtcTick* pTick);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_RTC_H */
