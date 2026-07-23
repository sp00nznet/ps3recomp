/*
 * ps3recomp - Semaphore syscalls (implementation)
 */

#include "sys_semaphore.h"
#include "../ps3_log.h"
#include "sys_timer.h"   /* lv2_usec_deadline: sub-ms timed waits */
#include "../memory/vm.h"
#include <string.h>
#include <stdlib.h>      /* getenv (else the pointer return is truncated to int) */

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_sem_table_lock;
static int              s_sem_table_lock_init = 0;
#else
static pthread_mutex_t  s_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void sem_table_lock(void)
{
#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
    EnterCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_lock(&s_sem_table_lock);
#endif
}

static void sem_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_unlock(&s_sem_table_lock);
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

static uint32_t read_be32(uint32_t addr)
{
    uint32_t v; uint32_t* p = (uint32_t*)vm_to_host(addr);
    v = *p;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    v = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
        ((v <<  8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
#endif
    return v;
}

/* ---------------------------------------------------------------------------
 * lbp_hang_census (diagnostic; called from the hang watchdog)
 *
 * Reads the LBP resource-manager's three work-queue depths straight out of
 * guest memory and cross-references each queue's semaphore runtime value. The
 * manager pointer lives at guest [TOC(0x93BCC0)-25864] = 0x009357B8; each queue
 * struct has +0=base, +4=count, +8=capacity, +0x30=sem_id, +0x34=done flag.
 *
 * Reading:  count>0 with sem.value==0  => item enqueued but no post reached the
 *           consumer (lost wakeup / producer bug), OR the consumer left (done!=0)
 *           without draining. count>0 with sem.value>0 => a post is pending and
 *           unconsumed (the consumer has exited). all counts 0 => queues drained,
 *           the stall is in completion signalling, not the pump.
 * -----------------------------------------------------------------------*/
void lbp_hang_census(void)
{
    uint32_t mgr = read_be32(0x009357B8);
    fprintf(stderr, "\n[HANGCENSUS] resource-manager mgr=0x%08X\n", mgr);
    const uint32_t qoff[3] = { 0x3C, 0x40, 0x44 };
    int poke = getenv("POKESEM") ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        uint32_t q    = read_be32(mgr + qoff[i]);
        uint32_t base = read_be32(q + 0x00);
        uint32_t cnt  = read_be32(q + 0x04);
        uint32_t cap  = read_be32(q + 0x08);
        uint32_t sem  = read_be32(q + 0x30);
        /* the guest done flag is the BYTE at address q+0x34 = big-endian high byte */
        uint32_t done = (read_be32(q + 0x34) >> 24) & 0xFF;
        fprintf(stderr, "[HANGCENSUS] q[%d]@mgr+0x%02X ptr=0x%08X count=%u cap=%u sem=%u done=%u base=0x%08X\n",
                i, qoff[i], q, cnt, cap, sem, done, base);
        fprintf(stderr, "[HANGCENSUS]   raw:");
        for (uint32_t o = 0; o <= 0x34; o += 4) fprintf(stderr, " +%02X=%08X", o, read_be32(q + o));
        fprintf(stderr, "\n");
        if (sem >= 1 && sem <= SYS_SEMAPHORE_MAX) {
            sys_semaphore_info* qs = &g_sys_semaphores[sem - 1];
            fprintf(stderr, "[HANGCENSUS]   sem=%u runtime: active=%d value=%d max=%d\n",
                    sem, qs->active, qs->value, qs->max_value);
#ifdef _WIN32
            /* POKESEM experiment: if the guest queue holds more items than the
             * semaphore has been posted for, the wakeup was lost -- post the
             * deficit and see whether the consumer drains it and the load proceeds. */
            if (poke && qs->active && (int)cnt > qs->value) {
                int deficit = (int)cnt - qs->value;
                EnterCriticalSection(&qs->value_lock);
                qs->value += deficit;
                LeaveCriticalSection(&qs->value_lock);
                ReleaseSemaphore(qs->sem_handle, deficit, NULL);
                fprintf(stderr, "[HANGCENSUS]   *** POKED sem=%u by %d (count=%u > value) ***\n",
                        sem, deficit, cnt);
            }
#endif
        }
    }
#ifdef _WIN32
    /* POKE7=<id>: one-shot force-post of an arbitrary semaphore, to test whether a
     * starved gate (e.g. the completion sem tid0 polls) is merely a lost wakeup --
     * if the load then proceeds, the state was ready and only the signal was lost. */
    { const char* pk = getenv("POKE7");
      if (pk && *pk) {
        static int done_pk = 0;
        if (!done_pk) { done_pk = 1;
          int id = atoi(pk);
          if (id >= 1 && id <= SYS_SEMAPHORE_MAX && g_sys_semaphores[id-1].active) {
            sys_semaphore_info* ps = &g_sys_semaphores[id-1];
            EnterCriticalSection(&ps->value_lock); ps->value += 1; LeaveCriticalSection(&ps->value_lock);
            ReleaseSemaphore(ps->sem_handle, 1, NULL);
            fprintf(stderr, "[HANGCENSUS] *** FORCE-POSTED sem=%d by 1 (POKE7 test) ***\n", id);
          }
        }
      }
    }
#endif
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * lbp_unstick_once (diagnostic; called on a timer when UNSTICK is set)
 *
 * Tests the lost-wakeup hypothesis: the loader is a producer/consumer pipeline
 * whose "an item is ready / work is done" nudge posts are intermittently lost
 * (the hang is nondeterministic). Each tick:
 *   - for each of the 3 resource-manager queues, if the guest item count exceeds
 *     the semaphore value, post the deficit (a lost enqueue-post) so the consumer
 *     drains it and advances the resource one load-step;
 *   - nudge the top-level completion sem (id 7) so tid0 re-checks its predicate.
 * If the title then reaches the loading screen, the deadlock IS lost wakeups and
 * the real fix is to make the guest producer/consumer handshake race-free.
 * -----------------------------------------------------------------------*/
void lbp_unstick_once(void)
{
#ifdef _WIN32
    uint32_t mgr = read_be32(0x009357B8);
    if (!mgr) return;
    const uint32_t qoff[3] = { 0x3C, 0x40, 0x44 };
    for (int i = 0; i < 3; i++) {
        uint32_t q   = read_be32(mgr + qoff[i]);
        uint32_t cnt = read_be32(q + 0x04);
        uint32_t sem = read_be32(q + 0x30);
        if (sem >= 1 && sem <= SYS_SEMAPHORE_MAX && g_sys_semaphores[sem-1].active) {
            sys_semaphore_info* ps = &g_sys_semaphores[sem-1];
            if ((int)cnt > ps->value) {
                int d = (int)cnt - ps->value;
                EnterCriticalSection(&ps->value_lock); ps->value += d; LeaveCriticalSection(&ps->value_lock);
                ReleaseSemaphore(ps->sem_handle, d, NULL);
            }
        }
    }
    /* nudge the completion sem (id 7) so tid0 re-evaluates the load predicate */
    if (g_sys_semaphores[7-1].active) {
        sys_semaphore_info* ps = &g_sys_semaphores[7-1];
        if (ps->value < 2) {
            EnterCriticalSection(&ps->value_lock); ps->value += 1; LeaveCriticalSection(&ps->value_lock);
            ReleaseSemaphore(ps->sem_handle, 1, NULL);
        }
    }
#endif
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_create
 *
 * r3 = pointer to receive semaphore ID (u32*)
 * r4 = pointer to attribute struct
 * r5 = initial value
 * r6 = max value
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_create(ppu_context* ctx)
{
    uint32_t id_out_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr    = LV2_ARG_PTR(ctx, 1);
    int32_t  initial      = LV2_ARG_S32(ctx, 2);
    int32_t  max_val      = LV2_ARG_S32(ctx, 3);

    if (max_val <= 0 || initial < 0 || initial > max_val)
        return (int64_t)(int32_t)CELL_EINVAL;

    sem_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_SEMAPHORE_MAX; i++) {
        if (!g_sys_semaphores[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_semaphore_info* s = &g_sys_semaphores[slot];
    memset(s, 0, sizeof(*s));
    s->active    = 1;
    s->value     = initial;
    s->max_value = max_val;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        s->protocol = proto_be;
        /* name at offset 8 */
        memcpy(s->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    s->sem_handle = CreateSemaphoreA(NULL, initial, max_val, NULL);
    InitializeCriticalSection(&s->value_lock);
    if (s->sem_handle == NULL) {
        s->active = 0;
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#else
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
#endif

    uint32_t sem_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, sem_id);
    }

    /* Identify every semaphore at birth: name + creator's guest LR. The
     * LBP movie stall was a 2369-waits/0-posts semaphore whose OWNER took
     * grep archaeology to guess -- the name would have said it instantly. */
    { static int _n = 0;
      if (_n++ < 48)
          fprintf(stderr, "[sem] create id=%u name='%.8s' init=%d max=%d lr=0x%08X\n",
                  sem_id, s->name, initial, max_val, (uint32_t)ctx->lr); }

    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_destroy
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_destroy(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sem_table_lock();

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifdef _WIN32
    CloseHandle(s->sem_handle);
    DeleteCriticalSection(&s->value_lock);
#else
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
#endif

    s->active = 0;
    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_wait
 *
 * r3 = sem_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_wait(ppu_context* ctx)
{
    uint32_t sem_id     = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us = LV2_ARG_U64(ctx, 1);
    /* LBP_HLE_JOBDONE: the JobManagerWorker spins on sys_semaphore_wait/trywait
     * while waiting for SPU-job completions our lifted PM never writes; satisfy
     * them here (no-op unless the env is set + jobs are pending). */
    { extern void lbp_hle_complete_pending(void); lbp_hle_complete_pending(); }
    { static int _n = 0; if (getenv("SEMTID") && _n++ < 60000)
        fprintf(stderr, "[WAIT tid=%llu] semaphore_wait(sem=%u timeout=%llu)\n",
                (unsigned long long)ctx->thread_id, sem_id, (unsigned long long)timeout_us);
      else if (!getenv("SEMTID") && ps3_log_verbose())
        fprintf(stderr, "[WAIT] semaphore_wait(sem=%u timeout=%llu)\n", sem_id, (unsigned long long)timeout_us); }
    /* LBP_BREADCRUMB: every 500th wait, dump the per-tid indirect-call breadcrumb
     * table. Fires reliably DURING the loader hang (respump spins sem16 waits),
     * so two consecutive [BC] lines reveal which worker's count is FROZEN = the
     * job it's stuck in, and its last indirect-call target = that callback. */
    { static int bc=-2; if(bc==-2) bc=getenv("LBP_BREADCRUMB")?1:0;
      if(bc){ static long w=0; if((++w % 500)==0){ extern void lbp_breadcrumb_dump(const char*); lbp_breadcrumb_dump("semwait"); } } }
    /* SEMCHAIN: dump the guest call-chain at the main thread's sem=7 poll (the
     * "loading done" wait) to locate its loop -- is it meant to re-post sem=3? */
    if (getenv("SEMCHAIN") && sem_id == 7 && ctx->thread_id == 0) {
        static int _c = 0;
        if (_c++ < 2) { extern void ppu_log_host_chain(const char*); ppu_log_host_chain("sem7-wait-tid0"); }
    }
    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    DWORD result;
    if (timeout_us > 0 && timeout_us < 1000) {
        /* Sub-ms timed wait: WaitForSingleObject floors to 1 ms and the OS
         * rounds up to the timer tick; poll the handle (0-timeout try-acquire)
         * to a QPC deadline instead. Safe to poll: the semaphore count lives
         * in the Win32 kernel object, so a post between polls stays counted
         * and is simply picked up by the next try-acquire. */
        int64_t deadline = lv2_usec_deadline(timeout_us);
        for (;;) {
            result = WaitForSingleObject(s->sem_handle, 0);
            if (result != WAIT_TIMEOUT) break;
            if (lv2_deadline_passed(deadline)) break;
            SwitchToThread();
        }
    } else {
        DWORD ms = (timeout_us == 0) ? INFINITE : (DWORD)(timeout_us / 1000);
        result = WaitForSingleObject(s->sem_handle, ms);
    }
    if (result == WAIT_TIMEOUT) {
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
    if (result != WAIT_OBJECT_0) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    EnterCriticalSection(&s->value_lock);
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);

    if (timeout_us == 0) {
        while (s->value <= 0) {
            pthread_cond_wait(&s->cv, &s->mtx);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->value <= 0) {
            int rc = pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mtx);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_trywait
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_trywait(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);
    { extern void lbp_hle_complete_pending(void); lbp_hle_complete_pending(); }

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    DWORD result = WaitForSingleObject(s->sem_handle, 0);
    if (result == WAIT_TIMEOUT) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    if (result != WAIT_OBJECT_0) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    EnterCriticalSection(&s->value_lock);
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value <= 0) {
        pthread_mutex_unlock(&s->mtx);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_post
 *
 * r3 = sem_id
 * r4 = count (number to post)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_post(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);
    int32_t  count  = LV2_ARG_S32(ctx, 1);

    { static int _n=0; if (getenv("SEMTID") && _n < 60000)
        fprintf(stderr, "[WAKE tid=%llu] semaphore_post(sem=%u count=%d)\n",
                (unsigned long long)ctx->thread_id, sem_id, count);
      else if (!getenv("SEMTID") && _n < 40)
        fprintf(stderr, "[WAKE] semaphore_post(sem=%u count=%d)\n", sem_id, count);
      _n++; }
    /* SEMCHAIN: dump the guest call-chain at every sem=3 post (the sema the main
     * loading thread starves on) -- reveals which thread/loop produces it. */
    if (getenv("SEMCHAIN") && sem_id == 3) {
        static int _c = 0;
        if (_c++ < 3) { extern void ppu_log_host_chain(const char*);
            fprintf(stderr, "[SEMCHAIN] sem=3 post by tid=%llu\n", (unsigned long long)ctx->thread_id);
            ppu_log_host_chain("sem3-post"); }
    }

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (count <= 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    if (s->value + count > s->max_value) {
        LeaveCriticalSection(&s->value_lock);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    s->value += count;
    LeaveCriticalSection(&s->value_lock);

    ReleaseSemaphore(s->sem_handle, count, NULL);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value + count > s->max_value) {
        pthread_mutex_unlock(&s->mtx);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    s->value += count;
    /* Wake waiters */
    for (int i = 0; i < count; i++) {
        pthread_cond_signal(&s->cv);
    }
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_get_value
 *
 * r3 = sem_id
 * r4 = pointer to receive value (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_get_value(ppu_context* ctx)
{
    uint32_t sem_id   = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr = LV2_ARG_PTR(ctx, 1);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    int32_t val;
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    val = s->value;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    val = s->value;
    pthread_mutex_unlock(&s->mtx);
#endif

    if (out_addr != 0) {
        write_be32(out_addr, (uint32_t)val);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_semaphore_init(lv2_syscall_table* tbl)
{
    memset(g_sys_semaphores, 0, sizeof(g_sys_semaphores));

#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_SEMAPHORE_CREATE,    sys_semaphore_create);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_DESTROY,   sys_semaphore_destroy);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_WAIT,      sys_semaphore_wait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_TRYWAIT,   sys_semaphore_trywait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_POST,      sys_semaphore_post);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_GET_VALUE, sys_semaphore_get_value);
}
