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

#define SYS_TIMER_MAX  64

typedef struct sys_timer_info {
    int      active;
    int      running;
    uint64_t period_usec;
    int32_t  event_queue_id;   /* connected event queue */
    uint64_t source;           /* event source value */
    uint64_t data1;            /* event data1 */

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
