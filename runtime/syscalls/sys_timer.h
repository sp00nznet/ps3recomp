/*
 * ps3recomp - Timer and time syscalls
 */

#ifndef SYS_TIMER_H
#define SYS_TIMER_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* PS3 timebase frequency */
#define PS3_TIMEBASE_FREQ  79800000ULL

/* The guest mftb/mftbu clock: one global monotonic counter, host time scaled
 * to the PS3 timebase. Called from every lifted mftb site (ppu_lifter.py). */
uint64_t ppu_timebase_now(void);

#ifdef _WIN32
/* Sub-millisecond timed-wait support for the lv2 sync primitives.
 * SleepConditionVariableCS / WaitForSingleObject floor a timed wait to 1 ms,
 * and the OS then rounds that up to the timer tick -- a 30 us guest timeout
 * inflates ~50x even with timeBeginPeriod(1). Waits under 1 ms must instead
 * poll their wake predicate and yield to a QueryPerformanceCounter deadline
 * (same approach sys_timer_usleep already uses). */
int64_t lv2_usec_deadline(uint64_t usec);      /* QPC value 'usec' from now */
int     lv2_deadline_passed(int64_t deadline); /* 1 once QPC >= deadline */
#endif

#define SYS_TIMER_MAX  64

typedef struct sys_timer_info {
    int      active;
    int      running;
    uint64_t period_usec;
    int32_t  event_queue_id;   /* connected event queue */
    uint64_t source;           /* event source value */
    uint64_t data1;            /* event data1 */
    uint64_t data2;            /* event data2 (connect's fifth argument) */
    uint64_t next_expire;      /* next expiration, us on the timer clock */

#ifdef _WIN32
    HANDLE   timer_handle;
    HANDLE   stop_event;
    HANDLE   thread_handle;
#else
    pthread_t       thread;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int             stop_flag;
#endif

} sys_timer_info;

extern sys_timer_info g_sys_timers[SYS_TIMER_MAX];

/* Syscall handlers */
int64_t sys_timer_usleep(ppu_context* ctx);
int64_t sys_timer_sleep(ppu_context* ctx);
int64_t sys_time_get_current_time(ppu_context* ctx);
int64_t sys_time_get_timebase_frequency(ppu_context* ctx);
int64_t sys_timer_create(ppu_context* ctx);
int64_t sys_timer_destroy(ppu_context* ctx);
int64_t sys_timer_start(ppu_context* ctx);
int64_t sys_timer_stop(ppu_context* ctx);
int64_t sys_timer_connect_event_queue(ppu_context* ctx);
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx);

/* Registration */
void sys_timer_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_TIMER_H */
