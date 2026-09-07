/*
 * ps3recomp - Win32 waitable handles, WaitOnAddress and VirtualAlloc on POSIX
 *
 * The half of win32_compat.h that needs state shared across translation
 * units. See the header for the contract of each function; this file is about
 * how they are kept correct.
 *
 * One lock, s_lock, guards every waitable object. That is what makes
 * WaitForMultipleObjects(bWaitAll) mean what Win32 says it means -- either all
 * of the objects are consumed together or none is -- without a lock-ordering
 * scheme across objects. Each object has its own condition variable, used with
 * that shared mutex, so a SetEvent wakes only the threads waiting on that
 * event; a separate condition variable serves multi-object waiters, and every
 * state change broadcasts it only while such a waiter exists.
 *
 * Threads are created detached and tracked by a reference-counted object: one
 * reference belongs to the handle, one to the running thread, and whichever
 * lets go last frees it. That is why a thread routine may return after its
 * handle was closed, and why CloseHandle right after CreateThread -- the
 * "spawn and forget" pattern all over this tree -- leaks nothing.
 *
 * The same objects form the thread table that OpenThread and the toolhelp
 * snapshot walk, so a thread the shim did not create is adopted into one on
 * first use. Stopping a running thread and reading its registers is a
 * debugger's job done with a debugger's mechanism -- Mach on Darwin, a
 * real-time signal on Linux -- and the exception layer at the bottom of this
 * file is sigaction wearing the vectored-handler interface. The header
 * documents the contract of each; this file is about how they are kept
 * correct, and the ordering rules they must not break.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE          /* pthread_setaffinity_np, CPU_SET, REG_RIP */
#endif

#include "win32_compat.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#if defined(__APPLE__)
/* <sys/ucontext.h>, not <ucontext.h>: the latter is the deprecated
 * makecontext/swapcontext family and refuses to compile without _XOPEN_SOURCE,
 * while all that is wanted here is the ucontext_t a signal handler is given. */
#  include <sys/ucontext.h>
#  include <mach/mach.h>
#  include <mach/mach_vm.h>
#  include <mach/thread_act.h>
#  include <pthread/qos.h>
#else
#  include <semaphore.h>
#  include <ucontext.h>
#endif

/* The signal that parks a thread for SuspendThread on Linux. SIGRTMIN and the
 * two above it belong to the C library's own threading, and SIGRTMIN is not a
 * compile-time constant there, so this is only ever evaluated at run time. */
#if defined(__linux__)
#  define PS3_SUSPEND_SIGNAL (SIGRTMIN + 4)
#endif

/* ---------------------------------------------------------------------------
 * Last error
 * -----------------------------------------------------------------------*/
static _Thread_local DWORD t_last_error;
DWORD GetLastError(void)     { return t_last_error; }
void  SetLastError(DWORD e)  { t_last_error = e; }

/* ---------------------------------------------------------------------------
 * Waitable objects
 * -----------------------------------------------------------------------*/
enum { OBJ_EVENT = 1, OBJ_SEMAPHORE, OBJ_THREAD, OBJ_TIMER, OBJ_SNAPSHOT };
#define OBJ_MAGIC 0x57333234u   /* 'W324' */

typedef struct ps3_obj {
    uint32_t       magic;
    int            kind;
    int            refs;
    pthread_cond_t cv;                 /* used with s_lock */

    /* event */
    int            manual, signaled;

    /* semaphore */
    LONG           count, maximum;

    /* thread */
    pthread_t      pt;
    DWORD          tid;                /* kernel tid, known once it runs */
    int            started, done, suspend_count;
    int            adopted;            /* not created here; see thread_self() */
    int            priority;           /* THREAD_PRIORITY_*, applied at start too */
    DWORD_PTR      affinity;           /* 0 = unset */
    DWORD          exit_code;
    PS3_THREAD_FN  fn;
    unsigned     (*fn_crt)(void*);
    LPVOID         arg;
    struct ps3_obj* thr_next;          /* the thread table, guarded by s_lock */
    unsigned long long created_ft, exited_ft;   /* FILETIME units */
#if defined(__APPLE__)
    mach_port_t    port;               /* the thread's Mach port, once it runs */
#else
    sem_t          park_ack, park_go;  /* the suspend handshake, both ways */
    CONTEXT        park_ctx;           /* saved by the parking handler */
    int            park_ctx_dirty;     /* SetThreadContext wants it put back */
    int            park_ready;         /* the two semaphores exist */
#endif

    /* waitable timer */
    unsigned long long due_ns;         /* CLOCK_MONOTONIC */
    unsigned long long period_ns;
    int            timer_set;

    /* toolhelp snapshot */
    DWORD*         snap_tids;
    int            snap_count, snap_pos;
} ps3_obj;

static pthread_mutex_t s_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_multi = PTHREAD_COND_INITIALIZER;
static int             s_multi_waiters;
static ps3_obj*        s_threads;                 /* the thread table head */

static _Thread_local ps3_obj* t_current_thread;   /* set by the trampoline */

static ps3_obj* obj_new(int kind)
{
    ps3_obj* o = (ps3_obj*)calloc(1, sizeof *o);
    if (!o) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->magic = OBJ_MAGIC;
    o->kind  = kind;
    o->refs  = 1;
    pthread_cond_init(&o->cv, NULL);
    return o;
}

static ps3_obj* obj_of(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentThread() || h == GetCurrentProcess())
        return NULL;
    ps3_obj* o = (ps3_obj*)h;
    return o->magic == OBJ_MAGIC ? o : NULL;
}

/* Call with s_lock held. */
static void obj_release_locked(ps3_obj* o)
{
    if (--o->refs > 0) return;
    if (o->kind == OBJ_THREAD) {
        for (ps3_obj** p = &s_threads; *p; p = &(*p)->thr_next)
            if (*p == o) { *p = o->thr_next; break; }
#if !defined(__APPLE__)
        if (o->park_ready) { sem_destroy(&o->park_ack); sem_destroy(&o->park_go); }
#endif
    }
    free(o->snap_tids);
    o->magic = 0;
    pthread_cond_destroy(&o->cv);
    free(o);
}

/* Something a waiter might be waiting for changed. Call with s_lock held. */
static void obj_changed_locked(ps3_obj* o)
{
    pthread_cond_broadcast(&o->cv);
    if (s_multi_waiters) pthread_cond_broadcast(&s_multi);
}

static unsigned long long mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

/* Now, in FILETIME units: 100 ns since 1601, which is what GetThreadTimes
 * reports its creation and exit stamps in. */
static unsigned long long now_ft(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void ft_store(FILETIME* out, unsigned long long v)
{
    if (!out) return;
    out->dwLowDateTime  = (DWORD)v;
    out->dwHighDateTime = (DWORD)(v >> 32);
}

/* Is the object signaled right now? For a timer that means its due time has
 * passed, which is why timed waits below also wake at the earliest due time.
 * Call with s_lock held. */
static int obj_signaled_locked(const ps3_obj* o, unsigned long long now)
{
    switch (o->kind) {
    case OBJ_EVENT:     return o->signaled;
    case OBJ_SEMAPHORE: return o->count > 0;
    case OBJ_THREAD:    return o->done;
    case OBJ_TIMER:     return o->timer_set && now >= o->due_ns;
    }
    return 0;
}

/* Take the object: what a successful wait does to it. Call with s_lock held. */
static void obj_consume_locked(ps3_obj* o, unsigned long long now)
{
    switch (o->kind) {
    case OBJ_EVENT:     if (!o->manual) o->signaled = 0; break;
    case OBJ_SEMAPHORE: o->count--; break;
    case OBJ_THREAD:    break;
    case OBJ_TIMER:
        if (o->period_ns) {
            /* Periodic: advance past now so a late waiter does not see a
             * burst of stale periods, but keep the phase. */
            do o->due_ns += o->period_ns; while (o->due_ns <= now);
        } else if (!o->manual) {
            o->timer_set = 0;
        }
        break;
    }
}

/* The earliest time at which `o` could become signaled by the clock alone,
 * or 0 if never. Call with s_lock held. */
static unsigned long long obj_next_due_locked(const ps3_obj* o)
{
    return (o->kind == OBJ_TIMER && o->timer_set) ? o->due_ns : 0ull;
}

/* Wait on `cv` (with s_lock) until `deadline_ns` (monotonic; 0 = forever),
 * capped by `due_ns` when non-zero. Returns 0 on wake, ETIMEDOUT past the
 * deadline. Call with s_lock held. */
static int wait_until_locked(pthread_cond_t* cv, unsigned long long deadline_ns,
                             unsigned long long due_ns)
{
    unsigned long long target = deadline_ns;
    if (due_ns && (!target || due_ns < target)) target = due_ns;
    if (!target) return pthread_cond_wait(cv, &s_lock);

    unsigned long long now = mono_ns();
    if (deadline_ns && now >= deadline_ns) return ETIMEDOUT;
    if (now >= target) return 0;               /* a timer came due: re-check */

    /* pthread_cond_timedwait takes an absolute CLOCK_REALTIME time (Darwin has
     * no condattr clock), so convert the monotonic remainder. */
    unsigned long long remain = target - now;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(remain / 1000000000ull);
    ts.tv_nsec += (long)(remain % 1000000000ull);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int rc = pthread_cond_timedwait(cv, &s_lock, &ts);
    /* Only the caller's own deadline is a timeout. Waking at a timer's due
     * time, or early through REALTIME/MONOTONIC jitter, means "look again". */
    if (rc == ETIMEDOUT && (!deadline_ns || mono_ns() < deadline_ns)) rc = 0;
    return rc;
}

static unsigned long long deadline_from_ms(DWORD ms)
{
    return ms == INFINITE ? 0ull : mono_ns() + (unsigned long long)ms * 1000000ull;
}

/* --- events ---------------------------------------------------------------- */
HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
    (void)sa; (void)name;
    ps3_obj* o = obj_new(OBJ_EVENT);
    if (!o) return NULL;
    o->manual   = manual_reset ? 1 : 0;
    o->signaled = initial_state ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(void* sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name)
{ (void)name; return CreateEventA(sa, manual_reset, initial_state, NULL); }
HANDLE CreateEventExA(void* sa, LPCSTR name, DWORD flags, DWORD access)
{
    (void)access;
    return CreateEventA(sa, (flags & CREATE_EVENT_MANUAL_RESET) != 0,
                        (flags & CREATE_EVENT_INITIAL_SET) != 0, name);
}
BOOL SetEvent(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_EVENT) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->signaled = 1;
    obj_changed_locked(o);
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}
BOOL ResetEvent(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_EVENT) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->signaled = 0;
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}
BOOL PulseEvent(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_EVENT) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->signaled = 1;
    obj_changed_locked(o);
    o->signaled = 0;   /* waiters already woken re-check and consume nothing; Win32 PulseEvent is that unreliable too */
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- semaphores ------------------------------------------------------------ */
HANDLE CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    if (initial < 0 || maximum <= 0 || initial > maximum) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    ps3_obj* o = obj_new(OBJ_SEMAPHORE);
    if (!o) return NULL;
    o->count   = initial;
    o->maximum = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(void* sa, LONG initial, LONG maximum, LPCWSTR name)
{ (void)name; return CreateSemaphoreA(sa, initial, maximum, NULL); }
HANDLE CreateSemaphoreExA(void* sa, LONG initial, LONG maximum, LPCSTR name, DWORD flags, DWORD access)
{ (void)flags; (void)access; return CreateSemaphoreA(sa, initial, maximum, name); }
BOOL ReleaseSemaphore(HANDLE h, LONG count, LONG* previous)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_SEMAPHORE || count <= 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    if (o->count + count > o->maximum) {
        pthread_mutex_unlock(&s_lock);
        SetLastError(ERROR_INVALID_PARAMETER);       /* ERROR_TOO_MANY_POSTS on Win32 */
        return FALSE;
    }
    if (previous) *previous = o->count;
    o->count += count;
    obj_changed_locked(o);
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- waitable timers ------------------------------------------------------- */
HANDLE CreateWaitableTimerA(void* sa, BOOL manual_reset, LPCSTR name)
{
    (void)sa; (void)name;
    ps3_obj* o = obj_new(OBJ_TIMER);
    if (!o) return NULL;
    o->manual = manual_reset ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateWaitableTimerW(void* sa, BOOL manual_reset, LPCWSTR name)
{ (void)name; return CreateWaitableTimerA(sa, manual_reset, NULL); }
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms,
                      void* completion, void* arg, BOOL resume)
{
    (void)completion; (void)arg; (void)resume;
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_TIMER || !due) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    unsigned long long now = mono_ns();
    unsigned long long due_ns;
    if (due->QuadPart <= 0) {
        due_ns = now + (unsigned long long)(-due->QuadPart) * 100ull;   /* relative, 100 ns units */
    } else {
        /* Absolute FILETIME: distance from the wall clock, applied to the
         * monotonic clock. */
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        unsigned long long wall = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        unsigned long long tgt  = (unsigned long long)due->QuadPart;
        due_ns = tgt > wall ? now + (tgt - wall) * 100ull : now;
    }
    pthread_mutex_lock(&s_lock);
    o->due_ns    = due_ns;
    o->period_ns = period_ms > 0 ? (unsigned long long)period_ms * 1000000ull : 0ull;
    o->timer_set = 1;
    obj_changed_locked(o);       /* waiters re-arm their deadline against the new due time */
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}
BOOL CancelWaitableTimer(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_TIMER) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->timer_set = 0;
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- threads --------------------------------------------------------------- */

/* Apply a THREAD_PRIORITY_* level to the calling thread (or `pt`/`tid` when
 * it is another one). Best effort on every host: the point is to keep the
 * PPU, SPU-worker and RSX threads ahead of housekeeping, not to promise a
 * scheduler class. */
static void apply_priority(pthread_t pt, DWORD tid, int level, int is_self)
{
#if defined(__APPLE__)
    struct sched_param sp; int policy = SCHED_OTHER;
    if (pthread_getschedparam(pt, &policy, &sp) == 0) {
        int lo = sched_get_priority_min(policy), hi = sched_get_priority_max(policy);
        int def = (lo + hi) / 2;
        int p;
        if      (level >= THREAD_PRIORITY_TIME_CRITICAL) p = hi;
        else if (level <= THREAD_PRIORITY_IDLE)          p = lo;
        else                                             p = def + level * ((hi - def) / 4);
        if (p < lo) p = lo;
        if (p > hi) p = hi;
        sp.sched_priority = p;
        pthread_setschedparam(pt, policy, &sp);
    }
    /* QoS is what actually decides P-core vs E-core on Apple Silicon. Only the
     * calling thread can set its own; a thread created suspended gets it from
     * the trampoline, which runs on the new thread. */
    if (is_self) {
        qos_class_t q = QOS_CLASS_DEFAULT;
        if      (level >= THREAD_PRIORITY_ABOVE_NORMAL) q = QOS_CLASS_USER_INTERACTIVE;
        else if (level == THREAD_PRIORITY_NORMAL)       q = QOS_CLASS_USER_INITIATED;
        else if (level >= THREAD_PRIORITY_LOWEST)       q = QOS_CLASS_UTILITY;
        else                                            q = QOS_CLASS_BACKGROUND;
        pthread_set_qos_class_self_np(q, 0);
    }
    (void)tid;
#elif defined(__linux__)
    /* SCHED_OTHER has no per-thread priority, only nice, which Linux applies
     * per thread through setpriority(PRIO_PROCESS, tid). Lowering nice below
     * zero needs CAP_SYS_NICE or an RLIMIT_NICE grant; without it the call
     * fails and the thread simply stays at its inherited level. */
    int nice;
    if      (level >= THREAD_PRIORITY_TIME_CRITICAL) nice = -20;
    else if (level >= THREAD_PRIORITY_HIGHEST)       nice = -10;
    else if (level >= THREAD_PRIORITY_ABOVE_NORMAL)  nice = -5;
    else if (level == THREAD_PRIORITY_NORMAL)        nice = 0;
    else if (level >= THREAD_PRIORITY_BELOW_NORMAL)  nice = 5;
    else if (level >= THREAD_PRIORITY_LOWEST)        nice = 10;
    else                                             nice = 19;
    if (is_self) tid = (DWORD)syscall(SYS_gettid);
    if (tid) setpriority(PRIO_PROCESS, (id_t)tid, nice);
    (void)pt;
#else
    (void)pt; (void)tid; (void)level; (void)is_self;
#endif
}

static void apply_affinity(pthread_t pt, DWORD_PTR mask)
{
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set);
    for (unsigned i = 0; i < 8 * sizeof mask && i < CPU_SETSIZE; i++)
        if (mask & ((DWORD_PTR)1 << i)) CPU_SET(i, &set);
    pthread_setaffinity_np(pt, sizeof set, &set);
#else
    /* Apple Silicon has no thread affinity; QoS (above) is the lever there. */
    (void)pt; (void)mask;
#endif
}

static void thread_finish(ps3_obj* t, DWORD code)
{
    pthread_mutex_lock(&s_lock);
    t->exit_code = code;
    t->exited_ft = now_ft();
    t->done = 1;
    obj_changed_locked(t);
    obj_release_locked(t);       /* the running thread's reference */
    pthread_mutex_unlock(&s_lock);
}

/* Record what only the thread itself can tell us: its kernel tid, and on
 * Darwin the Mach port that thread_suspend and thread_get_state take. Both are
 * published before the CREATE_SUSPENDED gate, so a handle is fully usable the
 * moment the thread exists. Call with s_lock held. */
static void thread_publish_self_locked(ps3_obj* t)
{
    t->tid = GetCurrentThreadId();
#if defined(__APPLE__)
    t->port = pthread_mach_thread_np(pthread_self());
#endif
}

static void* thread_trampoline(void* p)
{
    ps3_obj* t = (ps3_obj*)p;
    t_current_thread = t;

    pthread_mutex_lock(&s_lock);
    thread_publish_self_locked(t);
    while (t->suspend_count > 0)                 /* CREATE_SUSPENDED gate */
        pthread_cond_wait(&t->cv, &s_lock);
    t->started = 1;
    int prio = t->priority; DWORD_PTR aff = t->affinity;
    pthread_mutex_unlock(&s_lock);

    if (prio != THREAD_PRIORITY_NORMAL) apply_priority(pthread_self(), t->tid, prio, 1);
    if (aff) apply_affinity(pthread_self(), aff);

    DWORD rc = t->fn ? t->fn(t->arg) : (DWORD)t->fn_crt(t->arg);
    t_current_thread = NULL;
    thread_finish(t, rc);
    return NULL;
}

/* Everything a thread record needs that is not about how the thread started:
 * the Linux park handshake and the creation stamp, plus the table link. Call
 * without s_lock; it takes it. Returns 0 if the semaphores could not be made. */
static int thread_arm(ps3_obj* t)
{
#if !defined(__APPLE__)
    if (sem_init(&t->park_ack, 0, 0) != 0) return 0;
    if (sem_init(&t->park_go, 0, 0) != 0) { sem_destroy(&t->park_ack); return 0; }
    t->park_ready = 1;
#endif
    t->created_ft = now_ft();
    pthread_mutex_lock(&s_lock);
    t->thr_next = s_threads;
    s_threads = t;
    pthread_mutex_unlock(&s_lock);
    return 1;
}

static HANDLE create_thread(SIZE_T stack, PS3_THREAD_FN fn, unsigned (*fn_crt)(void*),
                            LPVOID arg, DWORD flags, DWORD* out_tid)
{
    if (out_tid) *out_tid = 0;
    ps3_obj* t = obj_new(OBJ_THREAD);
    if (!t) return NULL;
    t->refs   = 2;               /* the handle, and the thread itself */
    t->fn     = fn;
    t->fn_crt = fn_crt;
    t->arg    = arg;
    t->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    if (!thread_arm(t)) {
        t->magic = 0; pthread_cond_destroy(&t->cv); free(t);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack) {
        /* Darwin rejects a size that is not a page multiple; both reject one
         * under the minimum. */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        if (page == 0) page = 4096;
        size_t sz = (stack + page - 1) & ~(page - 1);
        if (sz < (size_t)PTHREAD_STACK_MIN) sz = ((size_t)PTHREAD_STACK_MIN + page - 1) & ~(page - 1);
        pthread_attr_setstacksize(&attr, sz);
    }
    int rc = pthread_create(&t->pt, &attr, thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        /* Through the release path: the record is already in the thread table
         * and, off Darwin, owns two semaphores. */
        pthread_mutex_lock(&s_lock);
        t->refs = 1;
        obj_release_locked(t);
        pthread_mutex_unlock(&s_lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return (HANDLE)t;
}

HANDLE CreateThread(void* sa, SIZE_T stack, PS3_THREAD_FN fn, LPVOID arg, DWORD flags, DWORD* out_tid)
{ (void)sa; return create_thread(stack, fn, NULL, arg, flags, out_tid); }

uintptr_t _beginthreadex(void* sa, unsigned stack, unsigned (*fn)(void*), void* arg,
                         unsigned flags, unsigned* out_tid)
{
    (void)sa;
    DWORD tid = 0;
    HANDLE h = create_thread(stack, NULL, fn, arg, flags, &tid);
    if (out_tid) *out_tid = tid;
    return (uintptr_t)h;
}

/* --- adopting a thread the shim did not create ------------------------------
 *
 * main() is the case that matters: a runner duplicates GetCurrentThread() into
 * a real handle so a watchdog on another thread can freeze it and read its
 * program counter. There is no record for it, so one is made on demand.
 *
 * The record is owned by the thread itself and released by a TLS destructor at
 * its exit, which is what keeps a stale pthread_t -- and on Darwin a Mach port
 * that the kernel may since have handed to a different thread -- out of the
 * table. A handle taken while the thread lived keeps the record alive past
 * that, reporting `done`, exactly as a shim-created thread's does. */
static pthread_key_t  s_adopt_key;
static pthread_once_t s_adopt_once = PTHREAD_ONCE_INIT;
static int            s_adopt_key_ready;

static void adopt_dtor(void* p)
{
    ps3_obj* t = (ps3_obj*)p;
    t_current_thread = NULL;
    thread_finish(t, 0);
}
static void adopt_key_init(void)
{
    if (pthread_key_create(&s_adopt_key, adopt_dtor) == 0) s_adopt_key_ready = 1;
}

/* Drop the destructor's claim before finishing a record by hand, so the record
 * is not released twice. */
static void adopt_forget_self(void)
{
    if (s_adopt_key_ready) pthread_setspecific(s_adopt_key, NULL);
}

/* The calling thread's record, made if this is the first time it has been
 * asked for. NULL only if the allocation failed. */
static ps3_obj* thread_self(void)
{
    if (t_current_thread) return t_current_thread;
    pthread_once(&s_adopt_once, adopt_key_init);
    ps3_obj* t = obj_new(OBJ_THREAD);
    if (!t) return NULL;
    t->refs    = 1;                     /* the thread's own */
    t->pt      = pthread_self();
    t->started = 1;
    t->adopted = 1;
    if (!thread_arm(t)) {
        t->magic = 0; pthread_cond_destroy(&t->cv); free(t);
        return NULL;
    }
    pthread_mutex_lock(&s_lock);
    thread_publish_self_locked(t);
    pthread_mutex_unlock(&s_lock);
    t_current_thread = t;
    if (s_adopt_key_ready) pthread_setspecific(s_adopt_key, t);
    return t;
}

void ExitThread(DWORD code)
{
    ps3_obj* t = t_current_thread;
    t_current_thread = NULL;
    adopt_forget_self();
    if (t) thread_finish(t, code);
    pthread_exit(NULL);
}
void _endthreadex(unsigned code) { ExitThread((DWORD)code); }

BOOL GetExitCodeThread(HANDLE h, DWORD* code)
{
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD || !code) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    *code = t->done ? t->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- the register file ------------------------------------------------------
 *
 * A CONTEXT is filled from two kinds of source: a Mach thread state, and a
 * signal frame. Both the suspend machinery below and the exception layer at
 * the bottom of this file go through these, so the two cannot drift.
 *
 * Only what the header declares is carried. Whatever a caller asks for in
 * ContextFlags, one thread_get_state or one signal frame delivers all of it at
 * once, so everything is filled and ContextFlags reports that. */
#if defined(__APPLE__) && defined(__aarch64__)
typedef arm_thread_state64_t ps3_native_state;
#define PS3_NATIVE_STATE_FLAVOR ARM_THREAD_STATE64
#define PS3_NATIVE_STATE_COUNT  ARM_THREAD_STATE64_COUNT

static void ctx_from_native(CONTEXT* c, const arm_thread_state64_t* s)
{
    for (int i = 0; i < 29; i++) c->X[i] = s->__x[i];
    c->Pc   = (ULONGLONG)arm_thread_state64_get_pc(*s);
    c->Sp   = (ULONGLONG)arm_thread_state64_get_sp(*s);
    c->Fp   = (ULONGLONG)arm_thread_state64_get_fp(*s);
    c->Lr   = (ULONGLONG)arm_thread_state64_get_lr(*s);
    c->Cpsr = (DWORD)s->__cpsr;
}
static void ctx_to_native(arm_thread_state64_t* s, const CONTEXT* c, int all)
{
    arm_thread_state64_set_pc_fptr(*s, (void*)(uintptr_t)c->Pc);
    arm_thread_state64_set_sp(*s, c->Sp);
    if (!all) return;
    for (int i = 0; i < 29; i++) s->__x[i] = c->X[i];
    arm_thread_state64_set_fp(*s, c->Fp);
    arm_thread_state64_set_lr_fptr(*s, (void*)(uintptr_t)c->Lr);
    s->__cpsr = (__uint32_t)c->Cpsr;
}
#elif defined(__APPLE__) && defined(__x86_64__)
typedef x86_thread_state64_t ps3_native_state;
#define PS3_NATIVE_STATE_FLAVOR x86_THREAD_STATE64
#define PS3_NATIVE_STATE_COUNT  x86_THREAD_STATE64_COUNT

static void ctx_from_native(CONTEXT* c, const x86_thread_state64_t* s)
{
    c->Rax = s->__rax; c->Rbx = s->__rbx; c->Rcx = s->__rcx; c->Rdx = s->__rdx;
    c->Rsi = s->__rsi; c->Rdi = s->__rdi;
    c->R8  = s->__r8;  c->R9  = s->__r9;  c->R10 = s->__r10; c->R11 = s->__r11;
    c->R12 = s->__r12; c->R13 = s->__r13; c->R14 = s->__r14; c->R15 = s->__r15;
    c->Rip = s->__rip; c->Rsp = s->__rsp; c->Rbp = s->__rbp;
    c->EFlags = (DWORD)s->__rflags;
    c->Lr = 0;
}
static void ctx_to_native(x86_thread_state64_t* s, const CONTEXT* c, int all)
{
    s->__rip = c->Rip; s->__rsp = c->Rsp;
    if (!all) return;
    s->__rax = c->Rax; s->__rbx = c->Rbx; s->__rcx = c->Rcx; s->__rdx = c->Rdx;
    s->__rsi = c->Rsi; s->__rdi = c->Rdi;
    s->__r8  = c->R8;  s->__r9  = c->R9;  s->__r10 = c->R10; s->__r11 = c->R11;
    s->__r12 = c->R12; s->__r13 = c->R13; s->__r14 = c->R14; s->__r15 = c->R15;
    s->__rbp = c->Rbp;
    s->__rflags = c->EFlags;
}
#endif

static void ctx_from_uc(CONTEXT* c, const ucontext_t* uc)
{
    memset(c, 0, sizeof *c);
    c->ContextFlags = CONTEXT_FULL;
#if defined(__APPLE__)
    ctx_from_native(c, &uc->uc_mcontext->__ss);
#elif defined(__linux__) && defined(__x86_64__)
    const greg_t* g = uc->uc_mcontext.gregs;
    c->Rax = (ULONGLONG)g[REG_RAX]; c->Rbx = (ULONGLONG)g[REG_RBX];
    c->Rcx = (ULONGLONG)g[REG_RCX]; c->Rdx = (ULONGLONG)g[REG_RDX];
    c->Rsi = (ULONGLONG)g[REG_RSI]; c->Rdi = (ULONGLONG)g[REG_RDI];
    c->R8  = (ULONGLONG)g[REG_R8];  c->R9  = (ULONGLONG)g[REG_R9];
    c->R10 = (ULONGLONG)g[REG_R10]; c->R11 = (ULONGLONG)g[REG_R11];
    c->R12 = (ULONGLONG)g[REG_R12]; c->R13 = (ULONGLONG)g[REG_R13];
    c->R14 = (ULONGLONG)g[REG_R14]; c->R15 = (ULONGLONG)g[REG_R15];
    c->Rip = (ULONGLONG)g[REG_RIP]; c->Rsp = (ULONGLONG)g[REG_RSP];
    c->Rbp = (ULONGLONG)g[REG_RBP];
    c->EFlags = (DWORD)g[REG_EFL];
#elif defined(__linux__) && defined(__aarch64__)
    for (int i = 0; i < 29; i++) c->X[i] = uc->uc_mcontext.regs[i];
    c->Fp   = uc->uc_mcontext.regs[29];
    c->Lr   = uc->uc_mcontext.regs[30];
    c->Sp   = uc->uc_mcontext.sp;
    c->Pc   = uc->uc_mcontext.pc;
    c->Cpsr = (DWORD)uc->uc_mcontext.pstate;
#else
    (void)uc;
    c->ContextFlags = 0;
#endif
}

/* `all` false writes back only Pc and Sp, which is what continuing from an
 * exception handler means: the handler fixed the mapping and wants the same
 * instruction, or the next one, on the same stack. */
static void ctx_to_uc(ucontext_t* uc, const CONTEXT* c, int all)
{
#if defined(__APPLE__)
    ctx_to_native(&uc->uc_mcontext->__ss, c, all);
#elif defined(__linux__) && defined(__x86_64__)
    greg_t* g = uc->uc_mcontext.gregs;
    g[REG_RIP] = (greg_t)c->Rip; g[REG_RSP] = (greg_t)c->Rsp;
    if (!all) return;
    g[REG_RAX] = (greg_t)c->Rax; g[REG_RBX] = (greg_t)c->Rbx;
    g[REG_RCX] = (greg_t)c->Rcx; g[REG_RDX] = (greg_t)c->Rdx;
    g[REG_RSI] = (greg_t)c->Rsi; g[REG_RDI] = (greg_t)c->Rdi;
    g[REG_R8]  = (greg_t)c->R8;  g[REG_R9]  = (greg_t)c->R9;
    g[REG_R10] = (greg_t)c->R10; g[REG_R11] = (greg_t)c->R11;
    g[REG_R12] = (greg_t)c->R12; g[REG_R13] = (greg_t)c->R13;
    g[REG_R14] = (greg_t)c->R14; g[REG_R15] = (greg_t)c->R15;
    g[REG_RBP] = (greg_t)c->Rbp;
    g[REG_EFL] = (greg_t)c->EFlags;
#elif defined(__linux__) && defined(__aarch64__)
    uc->uc_mcontext.pc = c->Pc;
    uc->uc_mcontext.sp = c->Sp;
    if (!all) return;
    for (int i = 0; i < 29; i++) uc->uc_mcontext.regs[i] = c->X[i];
    uc->uc_mcontext.regs[29] = c->Fp;
    uc->uc_mcontext.regs[30] = c->Lr;
    uc->uc_mcontext.pstate   = c->Cpsr;
#else
    (void)uc; (void)c; (void)all;
#endif
}

/* --- stopping a running thread ----------------------------------------------
 *
 * Darwin does it with the Mach calls a debugger would use. Linux has no such
 * call, so the thread stops itself: a real-time signal parks it inside its own
 * handler until the resume, with the interrupted registers saved into the
 * record on the way in and any SetThreadContext put back on the way out.
 *
 * The park handshake runs entirely on two semaphores, both async-signal-safe,
 * and the handler never touches s_lock. That is deliberate: SuspendThread
 * holds s_lock across the handshake, so a handler that wanted the lock would
 * deadlock instantly, and a fault or a wait on the target thread must be able
 * to proceed to the point where the signal is taken.
 *
 * Which record the handler is for comes through a global rather than the
 * thread-local pointer: the only thread that can be mid-handshake is the one
 * SuspendThread is holding s_lock for, and a thread whose routine has already
 * returned has cleared its own thread-local while still being alive enough to
 * take the signal. */
#if !defined(__APPLE__)
static ps3_obj* s_park_target;               /* set under s_lock */
static int      s_park_installed;

static void sem_wait_uninterrupted(sem_t* s)
{
    while (sem_wait(s) != 0 && errno == EINTR) { }
}

static void park_handler(int sig, siginfo_t* si, void* uctx)
{
    (void)sig; (void)si;
    ps3_obj* t = (ps3_obj*)__atomic_load_n(&s_park_target, __ATOMIC_ACQUIRE);
    if (!t || !pthread_equal(pthread_self(), t->pt)) return;   /* not ours */

    /* The thread resumes exactly where it was interrupted, which may be between
     * a failed call and its errno check. sem_wait writes errno on every
     * interruption, so the handler has to put it back. */
    int saved_errno = errno;
    ucontext_t* uc = (ucontext_t*)uctx;
    ctx_from_uc(&t->park_ctx, uc);
    t->park_ctx_dirty = 0;
    sem_post(&t->park_ack);                  /* the registers are readable now */
    sem_wait_uninterrupted(&t->park_go);
    if (t->park_ctx_dirty) ctx_to_uc(uc, &t->park_ctx, 1);
    errno = saved_errno;
}

/* Call with s_lock held. Returns 0 if the thread could not be stopped. */
static int park_thread_locked(ps3_obj* t)
{
    if (!t->park_ready) return 0;
    if (!s_park_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = park_handler;
        sa.sa_flags     = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(PS3_SUSPEND_SIGNAL, &sa, NULL) != 0) return 0;
        s_park_installed = 1;
    }
    __atomic_store_n(&s_park_target, t, __ATOMIC_RELEASE);
    if (pthread_kill(t->pt, PS3_SUSPEND_SIGNAL) != 0) {
        __atomic_store_n(&s_park_target, NULL, __ATOMIC_RELEASE);
        return 0;
    }
    sem_wait_uninterrupted(&t->park_ack);
    __atomic_store_n(&s_park_target, NULL, __ATOMIC_RELEASE);
    return 1;
}

static void unpark_thread_locked(ps3_obj* t) { sem_post(&t->park_go); }
#endif /* !__APPLE__ */

/* Can this record be stopped by the host right now? A thread that has not run
 * its trampoline yet is held on the CREATE_SUSPENDED gate instead, and a
 * finished one cannot be held at all. Call with s_lock held. */
static int thread_stoppable_locked(const ps3_obj* t)
{
    return t->started && !t->done;
}

DWORD ResumeThread(HANDLE h)
{
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    pthread_mutex_lock(&s_lock);
    DWORD prev = (DWORD)t->suspend_count;
    if (t->suspend_count > 0 && --t->suspend_count == 0) {
        if (!t->started) {
            pthread_cond_broadcast(&t->cv);       /* the CREATE_SUSPENDED gate */
        } else if (!t->done) {
#if defined(__APPLE__)
            thread_resume(t->port);
#else
            unpark_thread_locked(t);
#endif
        }
    }
    pthread_mutex_unlock(&s_lock);
    return prev;
}

/* Returns the PREVIOUS suspend count, or (DWORD)-1 on failure -- the value
 * Win32 code already checks for. Suspending the CALLING thread is one of the
 * failures: Win32 lets a thread suspend itself and wait for a rescuer, but
 * here the caller holds s_lock, so it would take the whole shim down with it. */
DWORD SuspendThread(HANDLE h)
{
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    if (t == t_current_thread || pthread_equal(t->pt, pthread_self())) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (DWORD)-1;
    }
    pthread_mutex_lock(&s_lock);
    DWORD r = (DWORD)-1;
    if (!t->started) {
        r = (DWORD)t->suspend_count;              /* held on the gate instead */
        t->suspend_count++;
    } else if (thread_stoppable_locked(t)) {
        int ok = 1;
        if (t->suspend_count == 0) {
#if defined(__APPLE__)
            ok = (thread_suspend(t->port) == KERN_SUCCESS);
#else
            ok = park_thread_locked(t);
#endif
        }
        if (ok) { r = (DWORD)t->suspend_count; t->suspend_count++; }
    }
    pthread_mutex_unlock(&s_lock);
    if (r == (DWORD)-1) SetLastError(ERROR_INVALID_PARAMETER);
    return r;
}

/* Resolve a thread handle (real or the pseudo-handle) for the priority and
 * naming calls. Returns 0 if the handle is not a thread. */
static int resolve_thread(HANDLE h, pthread_t* pt, DWORD* tid, int* is_self, ps3_obj** obj)
{
    *obj = NULL;
    if (h == GetCurrentThread()) {
        *pt = pthread_self(); *tid = GetCurrentThreadId(); *is_self = 1;
        *obj = t_current_thread;
        return 1;
    }
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD) return 0;
    *pt = t->pt; *tid = t->tid; *is_self = (t == t_current_thread); *obj = t;
    return 1;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    int started = 1;
    if (t) {
        pthread_mutex_lock(&s_lock);
        t->priority = priority;
        started = t->started;
        pthread_mutex_unlock(&s_lock);
    }
    if (started) apply_priority(pt, tid, priority, self);   /* else the trampoline applies it */
    return TRUE;
}

int GetThreadPriority(HANDLE h)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) { SetLastError(ERROR_INVALID_HANDLE); return THREAD_PRIORITY_ERROR_RETURN; }
    if (!t) return THREAD_PRIORITY_NORMAL;
    pthread_mutex_lock(&s_lock);
    int p = t->priority;
    pthread_mutex_unlock(&s_lock);
    return p;
}

DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t) || !mask) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    DWORD_PTR prev = t && t->affinity ? t->affinity : (DWORD_PTR)1;
    int started = 1;
    if (t) {
        pthread_mutex_lock(&s_lock);
        t->affinity = mask;
        started = t->started;
        pthread_mutex_unlock(&s_lock);
    }
    if (started) apply_affinity(pt, mask);
    return prev;
}

DWORD SetThreadIdealProcessor(HANDLE h, DWORD ideal)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    (void)ideal;
    return 0;   /* advisory on Windows too; the previous "ideal" is unknowable here */
}

LONG SetThreadDescription(HANDLE h, PCWSTR description)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) return (LONG)0x80070006L;   /* E_HANDLE */
    char name[64] = {0};
    if (description) {
        size_t n = wcstombs(name, description, sizeof name - 1);
        if (n == (size_t)-1) name[0] = '\0';
    }
#if defined(__APPLE__)
    if (self) pthread_setname_np(name);             /* Darwin names only the caller */
#elif defined(__linux__)
    char short_name[16];                            /* the kernel's TASK_COMM_LEN */
    size_t n = strnlen(name, sizeof short_name - 1);
    memcpy(short_name, name, n);
    short_name[n] = '\0';
    pthread_setname_np(pt, short_name);
#else
    (void)pt;
#endif
    return 0;   /* S_OK */
}

BOOL  SetPriorityClass(HANDLE process, DWORD cls) { (void)process; (void)cls; return TRUE; }
DWORD GetPriorityClass(HANDLE process)            { (void)process; return NORMAL_PRIORITY_CLASS; }

/* --- thread records by handle ----------------------------------------------- */

/* The record a thread handle names. The current-thread pseudo handle adopts
 * the caller, so every one of the calls below works on a thread the shim did
 * not create. NULL if the handle is not a thread. */
static ps3_obj* thread_of(HANDLE h)
{
    if (h == GetCurrentThread()) return thread_self();
    ps3_obj* t = obj_of(h);
    return (t && t->kind == OBJ_THREAD) ? t : NULL;
}

BOOL GetThreadContext(HANDLE h, CONTEXT* ctx)
{
    ps3_obj* t = thread_of(h);
    if (!t || !ctx) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
#if defined(__APPLE__)
    pthread_mutex_lock(&s_lock);
    mach_port_t port = thread_stoppable_locked(t) ? t->port : MACH_PORT_NULL;
    pthread_mutex_unlock(&s_lock);
    if (port == MACH_PORT_NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    ps3_native_state st;
    mach_msg_type_number_t n = PS3_NATIVE_STATE_COUNT;
    if (thread_get_state(port, PS3_NATIVE_STATE_FLAVOR, (thread_state_t)&st, &n) != KERN_SUCCESS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->ContextFlags = CONTEXT_FULL;
    ctx_from_native(ctx, &st);
    return TRUE;
#else
    /* Only the parking handler has a frame to answer from. */
    pthread_mutex_lock(&s_lock);
    BOOL ok = FALSE;
    if (t->suspend_count > 0 && thread_stoppable_locked(t)) { *ctx = t->park_ctx; ok = TRUE; }
    pthread_mutex_unlock(&s_lock);
    if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
    return ok;
#endif
}

BOOL SetThreadContext(HANDLE h, const CONTEXT* ctx)
{
    ps3_obj* t = thread_of(h);
    if (!t || !ctx) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
#if defined(__APPLE__)
    pthread_mutex_lock(&s_lock);
    mach_port_t port = (t->suspend_count > 0 && thread_stoppable_locked(t)) ? t->port : MACH_PORT_NULL;
    pthread_mutex_unlock(&s_lock);
    if (port == MACH_PORT_NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

    /* Read, overwrite what a CONTEXT carries, write back: the flavour holds
     * state this struct has no room for, and none of it should change. */
    ps3_native_state st;
    mach_msg_type_number_t n = PS3_NATIVE_STATE_COUNT;
    if (thread_get_state(port, PS3_NATIVE_STATE_FLAVOR, (thread_state_t)&st, &n) != KERN_SUCCESS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ctx_to_native(&st, ctx, 1);
    if (thread_set_state(port, PS3_NATIVE_STATE_FLAVOR, (thread_state_t)&st, n) != KERN_SUCCESS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
#else
    /* Handed to the parking handler, which puts it back into the signal frame
     * as it returns. */
    pthread_mutex_lock(&s_lock);
    BOOL ok = FALSE;
    if (t->suspend_count > 0 && thread_stoppable_locked(t)) {
        t->park_ctx = *ctx;
        t->park_ctx_dirty = 1;
        ok = TRUE;
    }
    pthread_mutex_unlock(&s_lock);
    if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
    return ok;
#endif
}

HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid)
{
    (void)access; (void)inherit;
    if (tid == GetCurrentThreadId()) {
        ps3_obj* self = thread_self();          /* so main() can open itself */
        if (self) {
            pthread_mutex_lock(&s_lock);
            self->refs++;
            pthread_mutex_unlock(&s_lock);
            return (HANDLE)self;
        }
    }
    HANDLE h = NULL;
    pthread_mutex_lock(&s_lock);
    /* A record whose thread has not run yet has no id, and must not be found
     * by an id of zero. */
    for (ps3_obj* t = s_threads; t; t = t->thr_next)
        if (t->tid && t->tid == tid) { t->refs++; h = (HANDLE)t; break; }
    pthread_mutex_unlock(&s_lock);
    if (!h) SetLastError(ERROR_INVALID_PARAMETER);
    return h;
}

BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process,
                     HANDLE* out, DWORD access, BOOL inherit, DWORD options)
{
    (void)access; (void)inherit;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *out = NULL;
    if (src_process != GetCurrentProcess() || dst_process != GetCurrentProcess()) {
        SetLastError(ERROR_INVALID_PARAMETER);       /* one process, one address space */
        return FALSE;
    }
    ps3_obj* o = (src == GetCurrentThread()) ? thread_self() : obj_of(src);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->refs++;
    pthread_mutex_unlock(&s_lock);
    *out = (HANDLE)o;
    /* DUPLICATE_CLOSE_SOURCE on the pseudo handle is a no-op, as on Win32. */
    if ((options & DUPLICATE_CLOSE_SOURCE) && src != GetCurrentThread()) CloseHandle(src);
    return TRUE;
}

BOOL GetThreadTimes(HANDLE h, FILETIME* creation, FILETIME* exit,
                    FILETIME* kernel, FILETIME* user)
{
    ps3_obj* t = thread_of(h);
    if (!t) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

    pthread_mutex_lock(&s_lock);
    unsigned long long born = t->created_ft, died = t->exited_ft;
    /* A record whose thread has not started, or has ended, has no CPU clock to
     * read: the pthread_t is not valid in the first case and gone in the
     * second. The creation and exit stamps are the shim's own and always are. */
    int live = thread_stoppable_locked(t);
    pthread_t pt = t->pt;
#if defined(__APPLE__)
    mach_port_t port = t->port;
#endif
    pthread_mutex_unlock(&s_lock);

    ft_store(creation, born);
    ft_store(exit, died);
    ft_store(kernel, 0);
    ft_store(user, 0);
    if (!live) return TRUE;

#if defined(__APPLE__)
    struct thread_basic_info info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (port == MACH_PORT_NULL ||
        thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS)
        return TRUE;                             /* the stamps are still good */
    ft_store(kernel, (unsigned long long)info.system_time.seconds * 10000000ull
                     + (unsigned long long)info.system_time.microseconds * 10ull);
    ft_store(user,   (unsigned long long)info.user_time.seconds * 10000000ull
                     + (unsigned long long)info.user_time.microseconds * 10ull);
#else
    /* Per-thread user and system time apart needs /proc/self/task/<tid>/stat;
     * the CPU clock gives the sum, reported as user time. */
    clockid_t clk;
    struct timespec ts;
    if (pthread_getcpuclockid(pt, &clk) == 0 && clock_gettime(clk, &ts) == 0)
        ft_store(user, (unsigned long long)ts.tv_sec * 10000000ull
                       + (unsigned long long)ts.tv_nsec / 100ull);
#endif
    (void)pt;
    return TRUE;
}

/* --- toolhelp thread enumeration -------------------------------------------- */
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{
    if (!(flags & TH32CS_SNAPTHREAD) || (pid != 0 && pid != GetCurrentProcessId())) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    thread_self();               /* the caller belongs in its own snapshot */

    ps3_obj* s = obj_new(OBJ_SNAPSHOT);
    if (!s) return INVALID_HANDLE_VALUE;

    pthread_mutex_lock(&s_lock);
    int n = 0;
    for (ps3_obj* t = s_threads; t; t = t->thr_next) if (t->tid && !t->done) n++;
    DWORD* ids = n ? (DWORD*)calloc((size_t)n, sizeof *ids) : NULL;
    if (ids) {
        int i = 0;
        for (ps3_obj* t = s_threads; t && i < n; t = t->thr_next)
            if (t->tid && !t->done) ids[i++] = t->tid;
        n = i;
    } else {
        n = 0;
    }
    s->snap_tids  = ids;
    s->snap_count = n;
    s->snap_pos   = 0;
    pthread_mutex_unlock(&s_lock);
    return (HANDLE)s;
}

/* Call with s_lock held. */
static BOOL snap_fill_locked(ps3_obj* s, THREADENTRY32* e)
{
    if (s->snap_pos >= s->snap_count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD size = e->dwSize;
    memset(e, 0, sizeof *e);
    e->dwSize             = size ? size : (DWORD)sizeof *e;
    e->cntUsage           = 1;
    e->th32ThreadID       = s->snap_tids[s->snap_pos++];
    e->th32OwnerProcessID = GetCurrentProcessId();
    e->tpBasePri          = THREAD_PRIORITY_NORMAL;
    return TRUE;
}

BOOL Thread32First(HANDLE snapshot, THREADENTRY32* entry)
{
    ps3_obj* s = obj_of(snapshot);
    if (!s || s->kind != OBJ_SNAPSHOT || !entry) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    s->snap_pos = 0;
    BOOL ok = snap_fill_locked(s, entry);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

BOOL Thread32Next(HANDLE snapshot, THREADENTRY32* entry)
{
    ps3_obj* s = obj_of(snapshot);
    if (!s || s->kind != OBJ_SNAPSHOT || !entry) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    BOOL ok = snap_fill_locked(s, entry);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

/* --- waits ----------------------------------------------------------------- */
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    ps3_obj* o = obj_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    unsigned long long deadline = deadline_from_ms(ms);
    DWORD result = WAIT_TIMEOUT;

    pthread_mutex_lock(&s_lock);
    o->refs++;                   /* the object must outlive the wait even if closed meanwhile */
    for (;;) {
        unsigned long long now = mono_ns();
        if (obj_signaled_locked(o, now)) { obj_consume_locked(o, now); result = WAIT_OBJECT_0; break; }
        if (ms == 0) break;
        if (wait_until_locked(&o->cv, deadline, obj_next_due_locked(o)) == ETIMEDOUT) {
            /* One last look: the state may have changed exactly at the deadline. */
            now = mono_ns();
            if (obj_signaled_locked(o, now)) { obj_consume_locked(o, now); result = WAIT_OBJECT_0; }
            break;
        }
    }
    obj_release_locked(o);
    pthread_mutex_unlock(&s_lock);
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return result;
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{ (void)alertable; return WaitForSingleObject(h, ms); }

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms)
{
    enum { MAX_WAIT = 64 };   /* MAXIMUM_WAIT_OBJECTS */
    if (count == 0 || count > MAX_WAIT || !handles) { SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    ps3_obj* objs[MAX_WAIT];
    for (DWORD i = 0; i < count; i++) {
        objs[i] = obj_of(handles[i]);
        if (!objs[i]) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    }
    unsigned long long deadline = deadline_from_ms(ms);
    DWORD result = WAIT_TIMEOUT;

    pthread_mutex_lock(&s_lock);
    for (DWORD i = 0; i < count; i++) objs[i]->refs++;
    s_multi_waiters++;
    for (;;) {
        unsigned long long now = mono_ns();
        if (wait_all) {
            DWORD ready = 0;
            for (DWORD i = 0; i < count; i++) ready += obj_signaled_locked(objs[i], now) ? 1 : 0;
            if (ready == count) {
                for (DWORD i = 0; i < count; i++) obj_consume_locked(objs[i], now);
                result = WAIT_OBJECT_0;
                break;
            }
        } else {
            DWORD hit = count;
            for (DWORD i = 0; i < count; i++)
                if (obj_signaled_locked(objs[i], now)) { hit = i; break; }
            if (hit < count) { obj_consume_locked(objs[hit], now); result = WAIT_OBJECT_0 + hit; break; }
        }
        if (ms == 0) break;
        unsigned long long due = 0;
        for (DWORD i = 0; i < count; i++) {
            unsigned long long d = obj_next_due_locked(objs[i]);
            if (d && (!due || d < due)) due = d;
        }
        if (wait_until_locked(&s_multi, deadline, due) == ETIMEDOUT) break;
    }
    s_multi_waiters--;
    for (DWORD i = 0; i < count; i++) obj_release_locked(objs[i]);
    pthread_mutex_unlock(&s_lock);
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return result;
}

BOOL CloseHandle(HANDLE h)
{
    if (h == GetCurrentThread() || h == GetCurrentProcess()) return TRUE;   /* pseudo-handles */
    ps3_obj* o = obj_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    obj_release_locked(o);
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * WaitOnAddress / WakeByAddress
 * -----------------------------------------------------------------------*/
#define PL_BUCKETS 64
static struct { pthread_mutex_t m; pthread_cond_t c; } s_pl[PL_BUCKETS];
static pthread_once_t s_pl_once = PTHREAD_ONCE_INIT;

static void pl_init(void)
{
    for (int i = 0; i < PL_BUCKETS; i++) {
        pthread_mutex_init(&s_pl[i].m, NULL);
        pthread_cond_init(&s_pl[i].c, NULL);
    }
}
static unsigned pl_bucket(const volatile void* a)
{
    uintptr_t x = (uintptr_t)a;
    x ^= x >> 17; x *= 0x9E3779B1u; x ^= x >> 13;
    return (unsigned)(x % PL_BUCKETS);
}
static int pl_equal(const volatile void* a, const void* cmp, size_t size)
{
    switch (size) {
    case 1: return *(const volatile uint8_t*)a  == *(const uint8_t*)cmp;
    case 2: return *(const volatile uint16_t*)a == *(const uint16_t*)cmp;
    case 4: return *(const volatile uint32_t*)a == *(const uint32_t*)cmp;
    case 8: return *(const volatile uint64_t*)a == *(const uint64_t*)cmp;
    default: return memcmp((const void*)a, cmp, size) == 0;
    }
}

BOOL WaitOnAddress(volatile VOID* address, PVOID compare, SIZE_T size, DWORD ms)
{
    if (!address || !compare || (size != 1 && size != 2 && size != 4 && size != 8)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_once(&s_pl_once, pl_init);
    unsigned b = pl_bucket(address);
    pthread_mutex_lock(&s_pl[b].m);
    BOOL ok = TRUE;
    if (pl_equal(address, compare, size)) {
        int rc;
        if (ms == INFINITE) rc = pthread_cond_wait(&s_pl[b].c, &s_pl[b].m);
        else { struct timespec ts = ps3__deadline_ms(ms); rc = pthread_cond_timedwait(&s_pl[b].c, &s_pl[b].m, &ts); }
        /* Woken (by a wake on this bucket, or spuriously): report success and
         * let the caller re-check, as the Win32 contract already demands. A
         * timeout with the value now different is still a success. */
        if (rc == ETIMEDOUT && pl_equal(address, compare, size)) ok = FALSE;
    }
    pthread_mutex_unlock(&s_pl[b].m);
    if (!ok) SetLastError(ERROR_TIMEOUT);
    return ok;
}

void WakeByAddressAll(PVOID address)
{
    pthread_once(&s_pl_once, pl_init);
    unsigned b = pl_bucket(address);
    pthread_mutex_lock(&s_pl[b].m);
    pthread_cond_broadcast(&s_pl[b].c);
    pthread_mutex_unlock(&s_pl[b].m);
}
void WakeByAddressSingle(PVOID address) { WakeByAddressAll(address); }   /* see the header */

/* ---------------------------------------------------------------------------
 * Virtual memory
 * -----------------------------------------------------------------------*/
#define VA_REGIONS 512
static struct { void* base; size_t size; DWORD protect; } s_regions[VA_REGIONS];

static int prot_of(DWORD protect)
{
    switch (protect & 0xFFu) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_NONE;
    }
}

static size_t page_size(void)
{
    long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? (size_t)v : 4096u;
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    size_t pg = page_size();
    uintptr_t a0 = (uintptr_t)address & ~(pg - 1);
    size_t    sz = (((uintptr_t)address + size + pg - 1) & ~(pg - 1)) - a0;
    int prot = (type & MEM_COMMIT) ? prot_of(protect) : PROT_NONE;

    if (type & MEM_RESERVE) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#if defined(MAP_FIXED_NOREPLACE)
        if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
        void* p = mmap(address ? (void*)a0 : NULL, sz, prot, flags, -1, 0);
        if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        if (address && p != (void*)a0) {
            /* The hint was not honoured: the range is in use, which is the
             * Win32 failure. Never displace whatever lives there. */
            munmap(p, sz);
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < VA_REGIONS; i++)
            if (!s_regions[i].base) {
                s_regions[i].base = p; s_regions[i].size = sz; s_regions[i].protect = protect;
                break;
            }
        pthread_mutex_unlock(&s_lock);
        return p;
    }
    if (type & MEM_COMMIT) {
        if (!address) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
        if (mprotect((void*)a0, sz, prot) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
        return (LPVOID)a0;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return NULL;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type)
{
    size_t pg = page_size();
    if (type & MEM_RELEASE) {
        BOOL ok = FALSE;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < VA_REGIONS; i++) {
            if (s_regions[i].base == address && s_regions[i].base) {
                munmap(s_regions[i].base, s_regions[i].size);
                s_regions[i].base = NULL; s_regions[i].size = 0; s_regions[i].protect = 0;
                ok = TRUE;
                break;
            }
        }
        pthread_mutex_unlock(&s_lock);
        if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
        return ok;
    }
    if (type & MEM_DECOMMIT) {
        uintptr_t a0 = (uintptr_t)address & ~(pg - 1);
        size_t    sz = (((uintptr_t)address + size + pg - 1) & ~(pg - 1)) - a0;
        if (mprotect((void*)a0, sz, PROT_NONE) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        madvise((void*)a0, sz, MADV_DONTNEED);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, DWORD* old)
{
    size_t pg = page_size();
    uintptr_t a0 = (uintptr_t)address & ~(pg - 1);
    size_t    sz = (((uintptr_t)address + size + pg - 1) & ~(pg - 1)) - a0;
    if (old) *old = PAGE_READWRITE;
    if (mprotect((void*)a0, sz, prot_of(protect)) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

void GetSystemInfo(SYSTEM_INFO* si)
{
    if (!si) return;
    memset(si, 0, sizeof *si);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    si->dwPageSize              = (DWORD)page_size();
    si->dwAllocationGranularity = si->dwPageSize;
    si->dwNumberOfProcessors    = (DWORD)n;
    si->dwActiveProcessorMask   = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    si->lpMinimumApplicationAddress = (LPVOID)(uintptr_t)si->dwPageSize;
    si->lpMaximumApplicationAddress = (LPVOID)(uintptr_t)0x00007FFFFFFFFFFFull;
}

/* ---------------------------------------------------------------------------
 * Querying the address space
 * -----------------------------------------------------------------------*/

/* The mapping the OS has at `addr`. Returns 1 with base/size/prot filled, or 0
 * for an unmapped address with `next` set to where the following mapping
 * starts (0 if there is none above it). Never fails: an address with no answer
 * is an address with nothing mapped at it. */
static int os_region(uintptr_t addr, uintptr_t* base, size_t* size, int* prot,
                     uintptr_t* next)
{
    *base = 0; *size = 0; *prot = 0; *next = 0;
#if defined(__APPLE__)
    /* mach_vm_region searches UPWARDS from the address it is given and reports
     * where it landed, so one call answers both questions: a base above the
     * address means nothing is mapped at it, and that base is the gap's end. */
    mach_vm_address_t a = (mach_vm_address_t)addr;
    mach_vm_size_t    sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t name = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &cnt, &name);
    if (name != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), name);
    if (kr != KERN_SUCCESS) return 0;             /* nothing at or above it */
    if ((uintptr_t)a > addr) { *next = (uintptr_t)a; return 0; }
    *base = (uintptr_t)a;
    *size = (size_t)sz;
    *prot = ((info.protection & VM_PROT_READ)    ? PROT_READ  : 0)
          | ((info.protection & VM_PROT_WRITE)   ? PROT_WRITE : 0)
          | ((info.protection & VM_PROT_EXECUTE) ? PROT_EXEC  : 0);
    return 1;
#elif defined(__linux__)
    /* /proc/self/maps is sorted by address, so the first line that starts above
     * the address ends the gap it sits in. */
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (addr >= (uintptr_t)lo && addr < (uintptr_t)hi) {
            *base = (uintptr_t)lo;
            *size = (size_t)(hi - lo);
            *prot = (perms[0] == 'r' ? PROT_READ  : 0)
                  | (perms[1] == 'w' ? PROT_WRITE : 0)
                  | (perms[2] == 'x' ? PROT_EXEC  : 0);
            found = 1;
            break;
        }
        if ((uintptr_t)lo > addr) { *next = (uintptr_t)lo; break; }
    }
    fclose(f);
    return found;
#else
    (void)addr;
    return 0;
#endif
}

static DWORD win_prot_of(int prot)
{
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return PAGE_NOACCESS;
    if (prot & PROT_EXEC) {
        if (prot & PROT_WRITE) return PAGE_EXECUTE_READWRITE;
        if (prot & PROT_READ)  return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return PAGE_READONLY;
}

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION mbi, SIZE_T length)
{
    if (!mbi || length < sizeof *mbi) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    size_t pg = page_size();
    uintptr_t a = (uintptr_t)address & ~(uintptr_t)(pg - 1);

    uintptr_t base = 0, next = 0;
    size_t    size = 0;
    int       prot = 0;
    int mapped = os_region(a, &base, &size, &prot, &next);

    memset(mbi, 0, sizeof *mbi);
    if (!mapped) {
        mbi->BaseAddress = (PVOID)a;
        mbi->RegionSize  = (SIZE_T)(next > a ? next - a : (uintptr_t)pg);
        mbi->State       = MEM_FREE;
        mbi->Protect     = PAGE_NOACCESS;
        return sizeof *mbi;
    }
    mbi->BaseAddress = (PVOID)base;
    mbi->RegionSize  = (SIZE_T)size;
    mbi->Protect     = win_prot_of(prot);
    mbi->Type        = MEM_PRIVATE;
    /* The shim's model: a reservation is PROT_NONE and a commit made it
     * accessible, so that is what the protection reads back as. */
    mbi->State       = (prot == 0) ? MEM_RESERVE : MEM_COMMIT;

    /* AllocationBase and AllocationProtect are the reservation this belongs
     * to, which is a Win32 notion neither host keeps -- but the shim does, for
     * everything it reserved itself. */
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < VA_REGIONS; i++) {
        uintptr_t rb = (uintptr_t)s_regions[i].base;
        if (s_regions[i].base && a >= rb && a < rb + s_regions[i].size) {
            mbi->AllocationBase    = s_regions[i].base;
            mbi->AllocationProtect = s_regions[i].protect;
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    if (!mbi->AllocationBase) {
        mbi->AllocationBase    = (PVOID)base;
        mbi->AllocationProtect = mbi->Protect;
    }
    return sizeof *mbi;
}

/* Every Win32 protection that grants the access in `need`, as a mask: the
 * PAGE_* values are distinct bits, so one test covers all of them. */
#define PS3_PAGE_READABLE (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | \
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
#define PS3_PAGE_WRITABLE (PAGE_READWRITE | PAGE_WRITECOPY | \
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

/* One query per region rather than per page, so a walk across a mapping costs
 * one lookup. On Darwin that lookup is a Mach trap; on Linux it re-reads
 * /proc/self/maps, which is not something to put in a hot loop. */
static BOOL range_has_access(uintptr_t p, size_t len, DWORD need)
{
    if (len == 0) return TRUE;                    /* Win32: zero length is good */
    uintptr_t end = p + len;
    if (end < p) return FALSE;                    /* wrapped past the top */
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof mbi) == 0) return FALSE;
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & need)) return FALSE;
        uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return FALSE;        /* no progress: refuse to spin */
        p = region_end;
    }
    return TRUE;
}

BOOL IsBadReadPtr(LPCVOID p, UINT_PTR length)
{
    if (length == 0) return FALSE;
    if (!p) return TRUE;
    return range_has_access((uintptr_t)p, (size_t)length, PS3_PAGE_READABLE) ? FALSE : TRUE;
}

BOOL IsBadWritePtr(LPVOID p, UINT_PTR length)
{
    if (length == 0) return FALSE;
    if (!p) return TRUE;
    return range_has_access((uintptr_t)p, (size_t)length, PS3_PAGE_WRITABLE) ? FALSE : TRUE;
}

BOOL IsBadCodePtr(LPCVOID p)
{
    if (!p) return TRUE;
    return range_has_access((uintptr_t)p, 1, PS3_PAGE_READABLE) ? FALSE : TRUE;
}

/* ---------------------------------------------------------------------------
 * Vectored exception handlers over sigaction
 *
 * The dispatcher runs in a signal handler, so it takes no lock. The handler
 * list is a chain of nodes out of a fixed pool, published with release stores
 * and read with acquire loads; a node is never unlinked, only emptied, and an
 * empty node is reused when the next registration wants the position it is
 * already in -- which is the arm, disarm, re-arm cycle a watchpoint does.
 * Registrations are serialised by their own lock, never s_lock, so a fault on
 * a thread inside a wait cannot deadlock against a registration.
 * -----------------------------------------------------------------------*/
#define VEH_MAX      64
#define PS3_SIG_MAX  32     /* the four signals taken over all sit well below this */

typedef struct ps3_veh_node {
    struct ps3_veh_node*        next;
    PVECTORED_EXCEPTION_HANDLER fn;      /* NULL once removed */
} ps3_veh_node;

static ps3_veh_node    s_veh_pool[VEH_MAX];
static int             s_veh_used;                    /* under s_veh_lock */
static ps3_veh_node*   s_veh_head;                    /* atomic */
static LPTOP_LEVEL_EXCEPTION_FILTER s_top_filter;     /* atomic */
static pthread_mutex_t s_veh_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sigaction s_prev_action[PS3_SIG_MAX];
static int              s_prev_valid[PS3_SIG_MAX];
static int              s_sig_installed;

static const int s_veh_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };

/* The arm64 exception syndrome for the fault, where there is one to read. Zero
 * on x86-64, which reports through the page-fault error code instead. */
#if defined(__aarch64__)
static unsigned long long fault_esr(const ucontext_t* uc)
{
    if (!uc) return 0;
#if defined(__APPLE__)
    return (unsigned long long)uc->uc_mcontext->__es.__esr;
#elif defined(__linux__)
    /* The ESR arrives as one record in the chain of extension blocks the
     * kernel appends to the signal frame, terminated by a zero magic. */
    struct ps3_a64_hdr { uint32_t magic; uint32_t size; };
    const unsigned char* p   = (const unsigned char*)uc->uc_mcontext.__reserved;
    const unsigned char* lim = p + sizeof uc->uc_mcontext.__reserved;
    while ((size_t)(lim - p) >= sizeof(struct ps3_a64_hdr)) {
        struct ps3_a64_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.magic == 0 || h.size < sizeof h || (size_t)(lim - p) < h.size) break;
        if (h.magic == 0x45535201u) {                 /* ESR_MAGIC */
            unsigned long long esr = 0;
            if (h.size >= sizeof h + sizeof esr) memcpy(&esr, p + sizeof h, sizeof esr);
            return esr;
        }
        p += h.size;
    }
    return 0;
#else
    return 0;
#endif
}
/* Data abort from a lower or the same exception level: the two syndromes that
 * carry a faulting data address and a direction. */
static int esr_is_data_abort(unsigned long long esr)
{
    unsigned ec = (unsigned)((esr >> 26) & 0x3Fu);
    return ec == 0x24u || ec == 0x25u;
}
#endif

/* An alignment fault specifically, as opposed to every other reason a load or
 * store can trap. arm64 says so in the syndrome's fault status code. */
static int fault_is_alignment(const ucontext_t* uc)
{
#if defined(__aarch64__)
    unsigned long long esr = fault_esr(uc);
    return esr_is_data_abort(esr) && (esr & 0x3Fu) == 0x21u;
#else
    (void)uc;
    return 0;
#endif
}

/* Which Win32 exception a signal means.
 *
 * si_code is the natural discriminator and is used where it discriminates. It
 * does not on Darwin, which routes every EXC_BAD_ACCESS that reached a mapped
 * but inaccessible page to SIGBUS and leaves si_code at 1 -- the value that
 * spells BUS_ADRALN -- whether the access was misaligned or merely forbidden.
 * A PROT_NONE page is exactly that case, so taking si_code at its word there
 * would report an alignment fault for the commonest access violation there is.
 * The trap frame is asked instead. */
static DWORD exception_code_of(int sig, const siginfo_t* si, const ucontext_t* uc)
{
    int code = si ? si->si_code : 0;
    switch (sig) {
    case SIGSEGV:
        return EXCEPTION_ACCESS_VIOLATION;
    case SIGBUS:
        if (fault_is_alignment(uc)) return EXCEPTION_DATATYPE_MISALIGNMENT;
#if !defined(__APPLE__)
        if (code == BUS_ADRALN) return EXCEPTION_DATATYPE_MISALIGNMENT;
        if (code == BUS_OBJERR) return EXCEPTION_IN_PAGE_ERROR;
#endif
        return EXCEPTION_ACCESS_VIOLATION;
    case SIGILL:
        if (code == ILL_PRVOPC || code == ILL_PRVREG) return EXCEPTION_PRIV_INSTRUCTION;
        return EXCEPTION_ILLEGAL_INSTRUCTION;
    case SIGFPE:
        switch (code) {
        case FPE_INTDIV: return EXCEPTION_INT_DIVIDE_BY_ZERO;
        case FPE_INTOVF: return EXCEPTION_INT_OVERFLOW;
        case FPE_FLTDIV: return EXCEPTION_FLT_DIVIDE_BY_ZERO;
        case FPE_FLTOVF: return EXCEPTION_FLT_OVERFLOW;
        case FPE_FLTUND: return EXCEPTION_FLT_UNDERFLOW;
        case FPE_FLTRES: return EXCEPTION_FLT_INEXACT_RESULT;
        case FPE_FLTSUB: return EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        default:         return EXCEPTION_FLT_INVALID_OPERATION;
        }
    default:
        return EXCEPTION_ILLEGAL_INSTRUCTION;
    }
}

/* Read or write? si_code does not say -- it separates unmapped from protected
 * -- so the answer comes from the trap frame: the ESR's WnR bit on arm64, the
 * page-fault error code's bit 1 on x86-64. Anything else reports a read, which
 * is the direction that claims less. */
static int fault_is_write(const ucontext_t* uc)
{
    if (!uc) return 0;
#if defined(__aarch64__)
    unsigned long long esr = fault_esr(uc);
    return esr_is_data_abort(esr) && (esr & (1ull << 6)) != 0;   /* WnR */
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uc->uc_mcontext->__es.__err & 2u) != 0;
#elif defined(__linux__) && defined(__x86_64__)
    return ((unsigned long long)uc->uc_mcontext.gregs[REG_ERR] & 2ull) != 0;
#else
    (void)uc;
    return 0;
#endif
}

/* Hand the signal to whoever had it before the shim, in the form they
 * registered it, and otherwise restore the default and return so the faulting
 * instruction re-runs and the process dies as it would have. This is the same
 * sequence runtime/ppu/ppu_loader.cpp uses for its own SIGSEGV handler, which
 * is what lets the two compose in either installation order. */
static void veh_chain_previous(int sig, siginfo_t* si, void* uctx)
{
    if (sig > 0 && sig < PS3_SIG_MAX && s_prev_valid[sig]) {
        struct sigaction prev = s_prev_action[sig];
        if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
            prev.sa_sigaction(sig, si, uctx);
            return;
        }
        if (prev.sa_handler && prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
            prev.sa_handler(sig);
            return;
        }
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
}

static void veh_signal_handler(int sig, siginfo_t* si, void* uctx)
{
    /* A handler that continues execution puts the thread back where it faulted,
     * so nothing this dispatcher does may be visible there -- errno included. */
    int saved_errno = errno;
    ucontext_t* uc = (ucontext_t*)uctx;
    CONTEXT ctx;
    ctx_from_uc(&ctx, uc);

    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof er);
    er.ExceptionCode    = exception_code_of(sig, si, uc);
    er.ExceptionAddress = (PVOID)(uintptr_t)ctx.Pc;
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        er.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
        er.NumberParameters = 2;
        er.ExceptionInformation[0] = uc && fault_is_write(uc) ? 1u : 0u;
        er.ExceptionInformation[1] = (ULONG_PTR)(uintptr_t)(si ? si->si_addr : NULL);
    }

    EXCEPTION_POINTERS ep;
    ep.ExceptionRecord = &er;
    ep.ContextRecord   = &ctx;

    for (ps3_veh_node* n = (ps3_veh_node*)__atomic_load_n(&s_veh_head, __ATOMIC_ACQUIRE);
         n; n = (ps3_veh_node*)__atomic_load_n(&n->next, __ATOMIC_ACQUIRE)) {
        PVECTORED_EXCEPTION_HANDLER fn =
            (PVECTORED_EXCEPTION_HANDLER)__atomic_load_n(&n->fn, __ATOMIC_ACQUIRE);
        if (!fn) continue;
        if (fn(&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            ctx_to_uc(uc, &ctx, 0);
            errno = saved_errno;
            return;
        }
    }

    LPTOP_LEVEL_EXCEPTION_FILTER top =
        (LPTOP_LEVEL_EXCEPTION_FILTER)__atomic_load_n(&s_top_filter, __ATOMIC_ACQUIRE);
    if (top && top(&ep) == EXCEPTION_CONTINUE_EXECUTION) {
        ctx_to_uc(uc, &ctx, 0);
        errno = saved_errno;
        return;
    }

    veh_chain_previous(sig, si, uctx);
}

/* Call with s_veh_lock held. */
static void veh_install_locked(void)
{
    if (s_sig_installed) return;
    s_sig_installed = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = veh_signal_handler;
    /* SA_ONSTACK so a process that installed a sigaltstack of its own can
     * still handle a stack overflow; the shim installs none itself. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof s_veh_signals / sizeof *s_veh_signals; i++) {
        int sig = s_veh_signals[i];
        struct sigaction prev;
        if (sigaction(sig, &sa, &prev) == 0) {
            s_prev_action[sig] = prev;
            s_prev_valid[sig]  = 1;
        }
    }
}

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
    if (!handler) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    pthread_mutex_lock(&s_veh_lock);
    veh_install_locked();

    /* An emptied node already sitting where this registration wants to go can
     * take it; that is what keeps arm/disarm/re-arm from consuming the pool. */
    ps3_veh_node* reuse = NULL;
    if (first) {
        if (s_veh_head && !s_veh_head->fn) reuse = s_veh_head;
    } else {
        ps3_veh_node* tail = s_veh_head;
        while (tail && tail->next) tail = tail->next;
        if (tail && !tail->fn) reuse = tail;
    }
    if (reuse) {
        __atomic_store_n(&reuse->fn, handler, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&s_veh_lock);
        return (PVOID)reuse;
    }

    if (s_veh_used >= VEH_MAX) {
        pthread_mutex_unlock(&s_veh_lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    ps3_veh_node* n = &s_veh_pool[s_veh_used++];
    n->fn = handler;
    if (first) {
        n->next = s_veh_head;
        __atomic_store_n(&s_veh_head, n, __ATOMIC_RELEASE);
    } else {
        n->next = NULL;
        if (!s_veh_head) {
            __atomic_store_n(&s_veh_head, n, __ATOMIC_RELEASE);
        } else {
            ps3_veh_node* tail = s_veh_head;
            while (tail->next) tail = tail->next;
            __atomic_store_n(&tail->next, n, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&s_veh_lock);
    return (PVOID)n;
}

ULONG RemoveVectoredExceptionHandler(PVOID handle)
{
    ps3_veh_node* n = (ps3_veh_node*)handle;
    if (!n || n < s_veh_pool || n >= s_veh_pool + VEH_MAX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    pthread_mutex_lock(&s_veh_lock);
    ULONG had = n->fn ? 1u : 0u;
    __atomic_store_n(&n->fn, (PVECTORED_EXCEPTION_HANDLER)NULL, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s_veh_lock);
    if (!had) SetLastError(ERROR_INVALID_HANDLE);
    return had;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    pthread_mutex_lock(&s_veh_lock);
    veh_install_locked();
    pthread_mutex_unlock(&s_veh_lock);
    return (LPTOP_LEVEL_EXCEPTION_FILTER)
        __atomic_exchange_n(&s_top_filter, filter, __ATOMIC_ACQ_REL);
}

#else  /* _WIN32 */
/* Nothing to build: the header is a passthrough to <windows.h> there. */
typedef int ps3_win32_compat_not_needed_on_windows;
#endif
