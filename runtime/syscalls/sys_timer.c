/*
 * ps3recomp - Timer and time syscalls (implementation)
 */

#include "sys_timer.h"
#include "sys_event.h"
#include "../memory/vm.h"
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_timer_info g_sys_timers[SYS_TIMER_MAX];

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    return now.QuadPart +
        (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}
#endif

/* ---------------------------------------------------------------------------
 * PPU timebase (mftb/mftbu) -- THE guest clock.
 *
 * One global monotonic counter scaled to the PS3 timebase (79.8 MHz),
 * anchored at first use. The old lifter emission was a per-call-site
 * static that advanced 16667 ticks PER READ -- not a clock at all: every
 * guest timing loop (media pacers, throttles, profilers) computed garbage
 * from it, and each call site had a PRIVATE counter that only moved when
 * polled. Overflow-safe split multiply (rem < qpf, so
 * rem * 79.8e6 < ~8e14 << 2^63).
 * -----------------------------------------------------------------------*/
uint64_t ppu_timebase_now(void)
{
#ifdef _WIN32
    static LONGLONG t0 = 0;
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!t0) t0 = now.QuadPart;   /* benign race: same anchor either way */
    uint64_t d = (uint64_t)(now.QuadPart - t0);
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * PS3_TIMEBASE_FREQ + (d % q) * PS3_TIMEBASE_FREQ / q;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * PS3_TIMEBASE_FREQ +
           (uint64_t)ts.tv_nsec * PS3_TIMEBASE_FREQ / 1000000000ull;
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_timer_usleep
 *
 * r3 = microseconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_usleep(ppu_context* ctx)
{
    uint64_t usec = LV2_ARG_U64(ctx, 0);
    { extern unsigned long long ps3_qpc_us(void);
      static int n=0; if (n++ < 60)
        fprintf(stderr, "[WAIT] t=%lluus timer_usleep(%llu us) lr=0x%08llX cia=0x%08llX\n", ps3_qpc_us(),
        (unsigned long long)usec, (unsigned long long)ctx->lr, (unsigned long long)ctx->cia); }
    /* PS3_POLLTOP=<seconds>: name the usleep poll sites.
     *
     * A title that is "hung" is usually not blocked on anything -- every thread
     * is spinning on sys_timer_usleep waiting for a flag, and [BLOCKSUM] only
     * says "syscall 141, 12s". This histograms the callers over a window and
     * prints the top ones, so the answer is a guest address rather than a
     * count. Keyed on ctx->lr: a host-stack backtrace through lifted code has
     * no unwind tables and invents chains (see the Guitar Hero III writeup),
     * and lr is the only guest chain that survives.
     *
     * Deliberately unlocked: the counts are a diagnostic, and a lost increment
     * under a race cannot change which site is at the top of a 30k-per-second
     * poll. */
    { static long s_pt = -1;
      if (s_pt < 0) { const char* e = getenv("PS3_POLLTOP");
                      s_pt = e ? (long)strtoul(e, 0, 10) : 0;
                      if (s_pt < 0) s_pt = 0; }
      if (s_pt > 0) {
        enum { PT_MAX = 128 };
        static uint32_t pt_lr[PT_MAX]; static uint32_t pt_n[PT_MAX];
        static uint64_t pt_us[PT_MAX]; static int pt_used = 0;
        static uint32_t pt_tid[PT_MAX];
        static unsigned long long pt_win = 0;
        uint32_t lr = (uint32_t)ctx->lr;
        int i = 0;
        for (; i < pt_used; i++) if (pt_lr[i] == lr) break;
        if (i == pt_used && pt_used < PT_MAX) {
            pt_lr[pt_used] = lr;
            /* The thread matters as much as the site: "who is waiting" is the
             * half that says whether a poll is the engine idling or the main
             * thread stuck. */
            pt_tid[pt_used] = (uint32_t)ctx->thread_id;
            pt_used++;
        }
        if (i < PT_MAX) { pt_n[i]++; pt_us[i] += usec; }
        { extern unsigned long long ps3_qpc_us(void);
          unsigned long long now = ps3_qpc_us();
          if (!pt_win) pt_win = now;
          else if (now - pt_win >= (unsigned long long)s_pt * 1000000ull) {
            fprintf(stderr, "--- usleep poll sites (last %lds) ---\n", s_pt);
            for (int k = 0; k < 8; k++) {
              int best = -1;
              for (int j = 0; j < pt_used; j++)
                if (pt_n[j] && (best < 0 || pt_n[j] > pt_n[best])) best = j;
              if (best < 0) break;
              fprintf(stderr, "  %8u calls  %6llums slept  tid=%-3u lr=0x%08X\n",
                      pt_n[best], (unsigned long long)(pt_us[best] / 1000),
                      pt_tid[best], pt_lr[best]);
              pt_n[best] = 0;
            }
            fflush(stderr);
            for (int j = 0; j < pt_used; j++) { pt_n[j] = 0; pt_us[j] = 0; }
            pt_win = now;
          } } } }

    /* PS3_WAIT_OBJ=<lr-hex>: when a usleep spin is reached from this return
     * address, dump the registers and the object they point at. A poll loop
     * tells you WHERE it is spinning; this tells you WHAT it is spinning on,
     * which is the part you actually need to find who never releases it. */
    { static long s_wo = -1;
      if (s_wo < 0) { const char* e = getenv("PS3_WAIT_OBJ");
                      s_wo = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_wo && (uint32_t)ctx->lr == (uint32_t)s_wo) {
        static int _n = 0;
        if (_n++ < 8) {
          /* Dump the words at EVERY plausible object register, not just r29.
           * Which register holds the object is per-title -- r29 was right for
           * the title this was written for and is a spin COUNTER in Guitar
           * Hero III, where the object is in r30/r31. Printing one guess makes
           * the tool silently report the wrong memory as "the thing it waits
           * on", which is worse than printing nothing. */
          fprintf(stderr, "[wait-obj] lr=0x%08X sleep=%lluus", (uint32_t)ctx->lr,
                  (unsigned long long)usec);
          static const int regs[] = { 28, 29, 30, 31, 3 };
          for (unsigned ri = 0; ri < sizeof regs / sizeof regs[0]; ri++) {
            const int r = regs[ri];
            const uint32_t o = (uint32_t)ctx->gpr[r];
            fprintf(stderr, "  r%d=0x%08X", r, o);
            /* Only deref something that looks like a guest pointer: a small
             * integer is a count, and dumping "memory at 4" is noise. */
            if (vm_base && o >= 0x10000u) {
              fprintf(stderr, "[");
              for (int w = 0; w < 4; w++) {
                const uint8_t* q = vm_base + o + (unsigned)w * 4u;
                uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                             ((uint32_t)q[2] << 8)  | (uint32_t)q[3];
                fprintf(stderr, "%s%08X", w ? " " : "", v);
              }
              fprintf(stderr, "]");
            }
          }
          fprintf(stderr, "\n"); fflush(stderr);
        } } }

    /* POLLSITE: resolve the host chain of the 1ms poller (LBP bringup) to
     * guest functions -- names the stage that is starving. */
    { static int _ps = -1; if (_ps < 0) _ps = getenv("POLLSITE") ? 12 : 0;
      if (_ps > 0 && usec == 1000) { _ps--;
        extern void ppu_log_host_chain(const char*);
        ppu_log_host_chain("usleep1ms"); } }

#ifdef _WIN32
    /* Use high-resolution sleep via waitable timer for better precision */
    if (usec >= 1000) {
        HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10); /* 100ns units, negative = relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            Sleep((DWORD)(usec / 1000));
        }
    } else if (usec > 0) {
        /* Sub-millisecond sleep. Sleep()/waitable timers round up to the
         * ~0.5-1 ms scheduler quantum, far too coarse for a guest that relies
         * on accurate microsecond pacing -- a short usleep() spin waiting on
         * another thread to make progress otherwise collapses to a single yield
         * and busy-spins at full speed (so the wait is effectively a no-op).
         * Busy-wait to a precise QPC deadline, yielding inside the spin
         * (SwitchToThread) so a sibling host thread the guest may be waiting on
         * still gets the core. Mirrors RPCS3's "Usleep Only" TSC busy-tail. */
        ensure_qpc_init();
        LARGE_INTEGER qpc_start, qpc_now;
        QueryPerformanceCounter(&qpc_start);
        const int64_t qpc_deadline = qpc_start.QuadPart +
            (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
        do {
            SwitchToThread();
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_now.QuadPart < qpc_deadline);
    }
#else
    if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(usec / 1000000);
        ts.tv_nsec = (long)((usec % 1000000) * 1000);
        nanosleep(&ts, NULL);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_sleep
 *
 * r3 = seconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_sleep(ppu_context* ctx)
{
    uint32_t sec = LV2_ARG_U32(ctx, 0);

#ifdef _WIN32
    Sleep(sec * 1000);
#else
    sleep(sec);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_current_time
 *
 * r3 = pointer to receive seconds (u64*)
 * r4 = pointer to receive nanoseconds (u64*)
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_current_time(ppu_context* ctx)
{
    uint32_t sec_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t nsec_addr = LV2_ARG_PTR(ctx, 1);

    uint64_t sec, nsec;

#ifdef _WIN32
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    /* Convert QPC to seconds + nanoseconds */
    sec  = (uint64_t)(now.QuadPart / s_qpc_freq.QuadPart);
    uint64_t remainder = (uint64_t)(now.QuadPart % s_qpc_freq.QuadPart);
    nsec = (remainder * 1000000000ULL) / (uint64_t)s_qpc_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec  = (uint64_t)ts.tv_sec;
    nsec = (uint64_t)ts.tv_nsec;
#endif

    if (sec_addr != 0) {
        write_be64(sec_addr, sec);
    }
    if (nsec_addr != 0) {
        write_be64(nsec_addr, nsec);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_timebase_frequency
 *
 * Returns the PS3 timebase frequency in r3.
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_timebase_frequency(ppu_context* ctx)
{
    (void)ctx;
    return (int64_t)PS3_TIMEBASE_FREQ;
}

/* ---------------------------------------------------------------------------
 * Periodic timer thread
 * -----------------------------------------------------------------------*/

/* Forward declaration */
static void timer_send_event(sys_timer_info* t);

#ifdef _WIN32
static DWORD WINAPI timer_thread_proc(LPVOID param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    while (t->running) {
        DWORD ms = (DWORD)(t->period_usec / 1000);
        if (ms == 0) ms = 1;

        DWORD result = WaitForSingleObject(t->stop_event, ms);
        if (result == WAIT_OBJECT_0) {
            break; /* stop signaled */
        }

        if (t->running) {
            timer_send_event(t);
        }
    }

    return 0;
}
#else
static void* timer_thread_proc(void* param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    pthread_mutex_lock(&t->mtx);
    while (!t->stop_flag) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(t->period_usec / 1000000);
        ts.tv_nsec += (long)((t->period_usec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        if (t->stop_flag) break;

        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            timer_send_event(t);
            pthread_mutex_lock(&t->mtx);
        }
    }
    pthread_mutex_unlock(&t->mtx);

    return NULL;
}
#endif

static void timer_send_event(sys_timer_info* t)
{
    if (t->event_queue_id <= 0 || t->event_queue_id > SYS_EVENT_QUEUE_MAX)
        return;

    sys_event_queue_info* q = &g_sys_event_queues[t->event_queue_id - 1];
    if (!q->active)
        return;

    sys_event_t evt;
    evt.source = t->source;
    evt.data1  = t->data1;
    evt.data2  = 0;
    evt.data3  = 0;

    /* Push event (ignore if queue full) */
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count < q->capacity) {
        q->buffer[q->tail] = evt;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
#ifdef _WIN32
        WakeConditionVariable(&q->not_empty);
#else
        pthread_cond_signal(&q->not_empty);
#endif
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* ---------------------------------------------------------------------------
 * sys_timer_create
 *
 * r3 = pointer to receive timer ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_timer_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);

    int slot = -1;
    for (int i = 0; i < SYS_TIMER_MAX; i++) {
        if (!g_sys_timers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_timer_info* t = &g_sys_timers[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->running = 0;
    t->event_queue_id = 0;

#ifdef _WIN32
    t->stop_event    = NULL;
    t->thread_handle = NULL;
    t->timer_handle  = NULL;
#else
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->stop_flag = 0;
#endif

    uint32_t timer_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, timer_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_destroy
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_destroy(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Stop if running */
    if (t->running) {
        t->running = 0;
#ifdef _WIN32
        if (t->stop_event) SetEvent(t->stop_event);
        if (t->thread_handle) {
            WaitForSingleObject(t->thread_handle, 3000);
            CloseHandle(t->thread_handle);
        }
        if (t->stop_event) CloseHandle(t->stop_event);
#else
        pthread_mutex_lock(&t->mtx);
        t->stop_flag = 1;
        pthread_cond_signal(&t->cv);
        pthread_mutex_unlock(&t->mtx);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
#endif
    }

    t->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_connect_event_queue
 *
 * r3 = timer_id
 * r4 = queue_id
 * r5 = source
 * r6 = data1
 * -----------------------------------------------------------------------*/
int64_t sys_timer_connect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    uint64_t source   = LV2_ARG_U64(ctx, 2);
    uint64_t data1    = LV2_ARG_U64(ctx, 3);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = (int32_t)queue_id;
    t->source         = source;
    t->data1          = data1;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_disconnect_event_queue
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_start
 *
 * r3 = timer_id
 * r4 = base_time (absolute start time, 0 = now)
 * r5 = period_usec
 * -----------------------------------------------------------------------*/
int64_t sys_timer_start(ppu_context* ctx)
{
    uint32_t timer_id    = LV2_ARG_U32(ctx, 0);
    /* uint64_t base_time = LV2_ARG_U64(ctx, 1); -- ignored for now */
    uint64_t period      = LV2_ARG_U64(ctx, 2);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (t->running)
        return (int64_t)(int32_t)CELL_EBUSY;

    if (period == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    t->period_usec = period;
    t->running     = 1;

#ifdef _WIN32
    t->stop_event    = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->thread_handle = CreateThread(NULL, 0, timer_thread_proc, t, 0, NULL);
#else
    t->stop_flag = 0;
    pthread_create(&t->thread, NULL, timer_thread_proc, t);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_stop
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_stop(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (!t->running)
        return CELL_OK;

    t->running = 0;

#ifdef _WIN32
    if (t->stop_event) SetEvent(t->stop_event);
    if (t->thread_handle) {
        WaitForSingleObject(t->thread_handle, 3000);
        CloseHandle(t->thread_handle);
        t->thread_handle = NULL;
    }
    if (t->stop_event) {
        CloseHandle(t->stop_event);
        t->stop_event = NULL;
    }
#else
    pthread_mutex_lock(&t->mtx);
    t->stop_flag = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    pthread_join(t->thread, NULL);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * NOTE: There are syscall number collisions in the existing table:
 *   SYS_TIMER_USLEEP (141) == SYS_EVENT_FLAG_WAIT (141)
 *   SYS_TIMER_SLEEP  (142) == SYS_EVENT_FLAG_TRYWAIT (142)
 *   SYS_TIME_GET_CURRENT_TIME (145) == SYS_EVENT_FLAG_CANCEL (145)
 * The event flag handlers are registered separately and will win.
 * Timer sleep/time functions should be called via wrapper functions
 * or fixed syscall numbers should be assigned.
 * For now, we register the timer-specific syscalls that don't collide.
 * -----------------------------------------------------------------------*/
void sys_timer_init(lv2_syscall_table* tbl)
{
    memset(g_sys_timers, 0, sizeof(g_sys_timers));

#ifdef _WIN32
    ensure_qpc_init();
#endif

    lv2_syscall_register(tbl, SYS_TIMER_CREATE,                   sys_timer_create);
    lv2_syscall_register(tbl, SYS_TIMER_DESTROY,                  sys_timer_destroy);
    lv2_syscall_register(tbl, SYS_TIMER_START,                    sys_timer_start);
    lv2_syscall_register(tbl, SYS_TIMER_STOP,                     sys_timer_stop);
    lv2_syscall_register(tbl, SYS_TIMER_CONNECT_EVENT_QUEUE,      sys_timer_connect_event_queue);
    lv2_syscall_register(tbl, SYS_TIMER_DISCONNECT_EVENT_QUEUE,   sys_timer_disconnect_event_queue);

    /* These have conflicting numbers with event flag syscalls.
     * Register them here -- last registration wins. If events are
     * registered after timers, the event handlers will override.
     * The runtime should dispatch timer_usleep/sleep/get_current_time
     * via direct function calls instead. */
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEBASE_FREQUENCY, sys_time_get_timebase_frequency);

    /* Register these but be aware of collisions */
    lv2_syscall_register(tbl, SYS_TIMER_USLEEP,            sys_timer_usleep);
    lv2_syscall_register(tbl, SYS_TIMER_SLEEP,             sys_timer_sleep);
    lv2_syscall_register(tbl, SYS_TIME_GET_CURRENT_TIME,   sys_time_get_current_time);
}
