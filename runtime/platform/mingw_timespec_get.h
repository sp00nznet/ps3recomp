/*
 * ps3recomp - C11 timespec_get for MinGW-w64 on msvcrt
 *
 * The UCRT has timespec_get and TIME_UTC; msvcrt.dll does not, and a MinGW-w64
 * <time.h> targeting it declares neither, so SPU_INTERP_STATS (spu_interp.c) and
 * SPU_JOBPROF (spu_workload.c) did not compile. Both only subtract two
 * readings, so this returns QueryPerformanceCounter time (monotonic, not UTC).
 * Include after <time.h> and, if the file uses it, after <windows.h>.
 */
#ifndef PS3RECOMP_MINGW_TIMESPEC_GET_H
#define PS3RECOMP_MINGW_TIMESPEC_GET_H
#include <time.h>
#if defined(__MINGW32__) && !defined(_UCRT) && !defined(TIME_UTC)
#define TIME_UTC 1
#ifdef _WINDOWS_   /* <windows.h> is in: use its prototypes */
static inline long long ps3_tsg_counter(long long* freq)
{
    LARGE_INTEGER c, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    *freq = f.QuadPart; return c.QuadPart;
}
#else
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long* c);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long* f);
static inline long long ps3_tsg_counter(long long* freq)
{
    long long c; QueryPerformanceFrequency(freq); QueryPerformanceCounter(&c); return c;
}
#endif
static inline int ps3_timespec_get(struct timespec* ts, int base)
{
    long long f = 1, c = ps3_tsg_counter(&f);
    if (f <= 0) f = 1;
    ts->tv_sec = (time_t)(c / f);
    ts->tv_nsec = (long)((c % f) * 1000000000LL / f);
    return base;
}
#define timespec_get ps3_timespec_get
#endif
#endif
