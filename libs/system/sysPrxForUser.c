/*
 * ps3recomp - sysPrxForUser HLE implementation
 *
 * Real host-backed implementation: lwmutex uses CRITICAL_SECTION/pthread_mutex,
 * lwcond uses CONDITION_VARIABLE/pthread_cond, threads use CreateThread/pthread.
 * Heap uses standard malloc with tracking.
 */

#include "sysPrxForUser.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32: translate + byte-swap */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal: Lightweight mutex table
 * -----------------------------------------------------------------------*/
#define MAX_LWMUTEX 256

typedef struct {
    int in_use;
    int recursive;
    char name[8];
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} LwMutexSlot;

static LwMutexSlot s_lwmutex[MAX_LWMUTEX];
static u32 s_lwmutex_next = 0;

/* Guards slot allocation in the create paths now that guest threads are
 * real host threads. Lock/unlock/signal stay lock-free: they only touch
 * the slot the caller already owns. */
#ifdef _WIN32
static SRWLOCK s_slot_lock = SRWLOCK_INIT;
static void slot_lock(void)   { AcquireSRWLockExclusive(&s_slot_lock); }
static void slot_unlock(void) { ReleaseSRWLockExclusive(&s_slot_lock); }
#else
static pthread_mutex_t s_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static void slot_lock(void)   { pthread_mutex_lock(&s_slot_lock); }
static void slot_unlock(void) { pthread_mutex_unlock(&s_slot_lock); }
#endif

/* Reset all lwmutex/lwcond state — call before CRT redirect to game main */
void sys_lwmutex_reset_all(void)
{
    for (u32 i = 0; i < MAX_LWMUTEX; i++) {
        if (s_lwmutex[i].in_use) {
#ifdef _WIN32
            DeleteCriticalSection(&s_lwmutex[i].cs);
#else
            pthread_mutex_destroy(&s_lwmutex[i].mtx);
#endif
        }
    }
    memset(s_lwmutex, 0, sizeof(s_lwmutex));
    s_lwmutex_next = 0;
}

/* ---------------------------------------------------------------------------
 * Internal: Lightweight cond table
 * -----------------------------------------------------------------------*/
#define MAX_LWCOND 256

typedef struct {
    int in_use;
    u32 lwmutex_id; /* index into s_lwmutex */
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cv;
#endif
} LwCondSlot;

static LwCondSlot s_lwcond[MAX_LWCOND];
static u32 s_lwcond_next = 0;

/* ---------------------------------------------------------------------------
 * Internal: Thread table
 * -----------------------------------------------------------------------*/
#define MAX_PRX_THREADS 64

typedef struct {
    int in_use;
    u64 thread_id;
    sys_ppu_thread_entry_t entry;
    u64 arg;
    u64 exitcode;
    int joined;
    int detached;
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t pt;
#endif
} PrxThreadSlot;

static PrxThreadSlot s_threads[MAX_PRX_THREADS];
static u64 s_next_thread_id = 0x10000; /* start high to avoid collision with sys_ppu_thread */

#ifdef _WIN32
static DWORD WINAPI prx_thread_entry(LPVOID param)
{
    PrxThreadSlot* t = (PrxThreadSlot*)param;
    t->entry(t->arg);
    return 0;
}
#else
static void* prx_thread_entry(void* param)
{
    PrxThreadSlot* t = (PrxThreadSlot*)param;
    t->entry(t->arg);
    return NULL;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal: Heap table
 * -----------------------------------------------------------------------*/
#define MAX_HEAPS 16

typedef struct {
    int in_use;
    u32 id;
} HeapSlot;

static HeapSlot s_heaps[MAX_HEAPS];
static u32 s_next_heap_id = 1;

/* ---------------------------------------------------------------------------
 * Thread management
 *
 * Thread functions (sys_ppu_thread_create, _exit, _join, _detach, _get_id,
 * _yield) are implemented in runtime/syscalls/sys_ppu_thread.c to avoid
 * duplication. They are declared in sysPrxForUser.h and linked from the
 * syscall implementation.
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

void sys_process_exit(s32 exitcode)
{
    printf("[sysPrxForUser] sys_process_exit(code=%d)\n", exitcode);
#ifdef _WIN32
    /* The RSX present thread runs at ~60Hz; a title that finishes in a few ms
     * would tear down before the first frame is ever presented. Hold
     * indefinitely with CELLMARK_HOLD so the last rendered frame is
     * visible / capturable. */
    if (getenv("CELLMARK_HOLD")) { for (;;) Sleep(1000); }
#endif
    /* YDKJ_NOEXIT (diagnostic): the title self-exits after ~2 frames on a
     * teardown assert triggered by incomplete SPU/SPURS state (a timing race).
     * Ignoring the exit on the calling thread lets the render loop keep running
     * so we can see whether sustained execution produces on-screen content. */
    if (getenv("YDKJ_NOEXIT")) {
        printf("[sysPrxForUser] sys_process_exit IGNORED (YDKJ_NOEXIT) -- parking thread\n");
        fflush(stdout);
        for (;;) {
#ifdef _WIN32
            Sleep(1000);
#else
            struct timespec ts = {1,0}; nanosleep(&ts,0);
#endif
        }
    }
    exit(exitcode);
}

s32 sys_process_getpid(void)
{
    return 1001;
}

s32 sys_process_get_number_of_object(u32 object_type, u32* count)
{
    (void)object_type;
    if (count) vm_write32((u32)(uintptr_t)count, 0);
    return CELL_OK;
}

s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags)
{
    (void)addr; (void)flags;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * String/memory functions
 * -----------------------------------------------------------------------*/

/* libsre and other system PRXs reach these sysPrxForUser CRT shims through
 * ps3_hle_call, which forwards the raw GUEST effective address in the arg
 * registers. Translate guest EA -> host pointer (vm_base + 32-bit EA; guest
 * NULL stays NULL) before touching memory. The title itself inlines its own
 * memcpy/memset/str*, so this path first appears once the real libsre is
 * loaded -- without translation memset(guest_ea, ...) writes to a bare host
 * address and AVs (observed: _sys_memset write fault during SPURS init). */
extern u8* vm_base;
static inline void* yz_g2h(const void* g)
{
    u32 ea = (u32)(uintptr_t)g;
    return ea ? (void*)(vm_base + ea) : (void*)0;
}

/* Guest-aware printf core: the varargs reach us as raw 64-bit register values
 * (the generic HLE adapter forwards gpr3..10), so a %s argument is a GUEST
 * effective address -- passing it straight to the host vprintf derefs it as a
 * host pointer and AVs (observed crashing the libspurs assert printer on the
 * "handler.c" filename). Walk the (already host-translated) format and emit each
 * conversion via the host snprintf, but translate %s args through vm_base. */
static int yz_format(char* out, size_t cap, const char* fmt, va_list ap)
{
    extern u8* vm_base;
    size_t p = 0;
    if (!fmt) { if (cap) out[0] = 0; return 0; }
    while (*fmt && p + 1 < cap) {
        if (*fmt != '%') { out[p++] = *fmt++; continue; }
        char spec[40]; int si = 0; spec[si++] = *fmt++;          /* '%' */
        while (*fmt && strchr("-+ #0123456789.*lhLqjzt", *fmt) && si < 38) spec[si++] = *fmt++;
        char c = *fmt ? *fmt++ : 0; spec[si++] = c; spec[si] = 0;
        char tmp[1024]; int n = 0;
        if (c == '%') { out[p++] = '%'; continue; }
        else if (c == 's') {
            u32 g = (u32)va_arg(ap, unsigned long long);
            const char* hs = g ? (const char*)(vm_base + g) : "(null)";
            n = snprintf(tmp, sizeof tmp, spec, hs);
        } else if (c == 'c') {
            int v = (int)va_arg(ap, unsigned long long); n = snprintf(tmp, sizeof tmp, spec, v);
        } else if (c == 'p') {
            u32 v = (u32)va_arg(ap, unsigned long long); n = snprintf(tmp, sizeof tmp, "0x%08X", v);
        } else if (c == 'f' || c == 'F' || c == 'g' || c == 'G' || c == 'e' || c == 'E') {
            union { unsigned long long u; double d; } u; u.u = va_arg(ap, unsigned long long);
            n = snprintf(tmp, sizeof tmp, spec, u.d);
        } else if (c) {
            unsigned long long v = va_arg(ap, unsigned long long);
            if (strstr(spec, "ll") || strstr(spec, "q")) n = snprintf(tmp, sizeof tmp, spec, (long long)v);
            else n = snprintf(tmp, sizeof tmp, spec, (int)v);
        }
        for (int k = 0; k < n && p + 1 < cap; k++) out[p++] = tmp[k];
    }
    out[p] = 0;
    return (int)p;
}

s32 _sys_printf(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char buf[2048];
    int ret = yz_format(buf, sizeof buf, (const char*)yz_g2h(fmt), ap);
    va_end(ap);
    printf("[PS3] %s", buf);
    return ret;
}

s32 _sys_sprintf(char* buf, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int ret = yz_format((char*)yz_g2h(buf), 0x10000, (const char*)yz_g2h(fmt), ap);
    va_end(ap);
    return ret;
}

s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int ret = yz_format((char*)yz_g2h(buf), size ? size : 1, (const char*)yz_g2h(fmt), ap);
    va_end(ap);
    return ret;
}

s32 _sys_strlen(const char* str)
{
    char* s = (char*)yz_g2h(str);
    if (!s) return 0;
    return (s32)strlen(s);
}

s32 _sys_strncpy(char* dst, const char* src, u32 size)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (!d || !s) return CELL_EFAULT;
    strncpy(d, s, size);
    return CELL_OK;
}

s32 _sys_strcat(char* dst, const char* src)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (!d || !s) return CELL_EFAULT;
    strcat(d, s);
    return CELL_OK;
}

s32 _sys_strcmp(const char* s1, const char* s2)
{
    const char* a = (const char*)yz_g2h(s1); const char* b = (const char*)yz_g2h(s2);
    if (!a || !b) return -1;
    return strcmp(a, b);
}

/* Returns the destination GUEST pointer (callers such as cellSpurs use the
 * return value), not the host pointer. */
char* _sys_strcpy(char* dst, const char* src)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (d && s) strcpy(d, s);
    return dst;
}

char* _sys_strncat(char* dst, const char* src, u32 size)
{
    char* d = (char*)yz_g2h(dst); const char* s = (const char*)yz_g2h(src);
    if (d && s) strncat(d, s, size);
    return dst;
}

s32 _sys_strncmp(const char* s1, const char* s2, u32 size)
{
    const char* a = (const char*)yz_g2h(s1); const char* b = (const char*)yz_g2h(s2);
    if (!a || !b) return -1;
    return strncmp(a, b, size);
}

void* _sys_memset(void* dst, s32 val, u32 size)
{
    void* d = yz_g2h(dst);
    if (d) memset(d, val, size);
    return dst;
}

void* _sys_memcpy(void* dst, const void* src, u32 size)
{
    void* d = yz_g2h(dst); const void* s = yz_g2h(src);
    if (d && s) memcpy(d, s, size);
    return dst;
}

s32 _sys_memcmp(const void* s1, const void* s2, u32 size)
{
    const void* a = yz_g2h(s1); const void* b = yz_g2h(s2);
    if (!a || !b) return 0;
    return memcmp(a, b, size);
}

s32 _sys_toupper(s32 c) { return toupper(c); }
s32 _sys_tolower(s32 c) { return tolower(c); }

/* ---------------------------------------------------------------------------
 * Lightweight mutex
 * -----------------------------------------------------------------------*/

extern u8* vm_base;  /* for guest-address diagnostics in the boot log */
#define YZ_GUEST_ADDR(p) ((u32)((u8*)(p) - vm_base))

/* Pointer PARAMETERS arrive as raw 32-bit guest EAs -- translate before any
 * deref. Everything below then works on host pointers, and YZ_GUEST_ADDR
 * round-trips correctly again. NULL is preserved so "attr ? ... : \"???\""
 * still means what it reads like.
 *
 * NOTE runtime/ppu/ppu_sysprx.cpp implements these SAME NIDs as ctx handlers
 * using vm_read32/vm_write32, and lbp/gen/ppu_hle_nids.cpp registers the ones
 * here -- whichever registers last wins. Both are now correct about
 * translation, so the order no longer decides whether it crashes. */
#define YZ_XLAT(p, T) ((p) = (p) ? (T)(void*)(vm_base + (u32)(uintptr_t)(p)) : (T)0)

s32 sys_lwmutex_create(sys_lwmutex_t_hle* lwmutex, const sys_lwmutex_attribute_t* attr)
{
    YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
    YZ_XLAT(attr, const sys_lwmutex_attribute_t*);
    printf("[sysPrxForUser] sys_lwmutex_create(name='%.8s', guest=0x%08X)\n",
           attr ? attr->name : "???", YZ_GUEST_ADDR(lwmutex));
    { extern char* getenv(const char*); static int _lt=-1; if(_lt<0)_lt=getenv("FLOW_LWMTRACE")?1:0;
      if(_lt){ extern unsigned int ppu_active_lr(void); printf("[LWMTRACE] create guest=0x%08X caller_lr=0x%08X\n", YZ_GUEST_ADDR(lwmutex), ppu_active_lr()); } }

    if (!lwmutex)
        return CELL_EFAULT;

    slot_lock();
    u32 idx = s_lwmutex_next;
    for (u32 i = 0; i < MAX_LWMUTEX; i++) {
        u32 slot = (idx + i) % MAX_LWMUTEX;
        if (!s_lwmutex[slot].in_use) {
            LwMutexSlot* m = &s_lwmutex[slot];
            m->in_use = 1;
            m->recursive = (attr && (attr->recursive & SYS_SYNC_RECURSIVE)) ? 1 : 0;
            if (attr)
                memcpy(m->name, attr->name, 8);

#ifdef _WIN32
            InitializeCriticalSection(&m->cs);
#else
            pthread_mutexattr_t mattr;
            pthread_mutexattr_init(&mattr);
            if (m->recursive)
                pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&m->mtx, &mattr);
            pthread_mutexattr_destroy(&mattr);
#endif

            memset(lwmutex, 0, sizeof(*lwmutex));
            lwmutex->sleep_queue = slot + 1; /* 1-based ID */
            s_lwmutex_next = (slot + 1) % MAX_LWMUTEX;
            slot_unlock();
            return CELL_OK;
        }
    }
    slot_unlock();
    return CELL_EAGAIN;
}

/* GT5P_LWMUTEX_GLOBAL: force every lwmutex onto one global critical section.
 *
 * A heap free list that ends up containing a LIVE block, with the damage
 * landing in a different place every run, is what broken mutual exclusion
 * looks like. If serialising all guest lwmutexes makes that corruption go
 * away, the per-mutex slot mapping is at fault; if it does not, the bug is in
 * single-threaded logic and the locks are exonerated. Diagnostic only --
 * serialising every lock in the title is not a fix. */
#ifdef _WIN32
static CRITICAL_SECTION s_lw_global;
static int s_lw_global_on = -1;
static int lw_global(void)
{
    if (s_lw_global_on < 0) {
        s_lw_global_on = getenv("GT5P_LWMUTEX_GLOBAL") ? 1 : 0;
        if (s_lw_global_on) {
            InitializeCriticalSection(&s_lw_global);
            fprintf(stderr, "[lwmutex] GLOBAL mode: all lwmutexes serialised\n");
        }
    }
    return s_lw_global_on;
}
#else
static int lw_global(void) { return 0; }
#endif

s32 sys_lwmutex_lock(sys_lwmutex_t_hle* lwmutex, u64 timeout)
{
    (void)timeout;
    YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use) {
#ifdef _WIN32
        printf("[sysPrxForUser] sys_lwmutex_lock FAIL guest=0x%08X "
               "sleep_queue=0x%08X (%s) [host tid %lu] -> ESRCH\n",
               YZ_GUEST_ADDR(lwmutex), lwmutex->sleep_queue,
               slot >= MAX_LWMUTEX ? "bad slot" : "slot not in use",
               GetCurrentThreadId());
#else
        printf("[sysPrxForUser] sys_lwmutex_lock FAIL guest=0x%08X "
               "sleep_queue=0x%08X (%s) -> ESRCH\n",
               YZ_GUEST_ADDR(lwmutex), lwmutex->sleep_queue,
               slot >= MAX_LWMUTEX ? "bad slot" : "slot not in use");
#endif
        { extern char* getenv(const char*); static int _lk=-1; if(_lk<0)_lk=getenv("FLOW_LOCKOK")?1:0;
          if(_lk) return CELL_OK; }   /* diag: treat uninit lwmutex as acquired, see if boot proceeds */
        return CELL_ESRCH;
    }

#ifdef _WIN32
    if (lw_global()) {
        EnterCriticalSection(&s_lw_global);
        lwmutex->lock_var = 1;
        lwmutex->recursive_count++;
        return CELL_OK;
    }
    if (!TryEnterCriticalSection(&s_lwmutex[slot].cs)) {
        fprintf(stderr, "[LWMTX] tid %lu BLOCKING on lwmutex slot %u (guest 0x%08X)\n",
                GetCurrentThreadId(), slot, YZ_GUEST_ADDR(lwmutex));
        EnterCriticalSection(&s_lwmutex[slot].cs);
        fprintf(stderr, "[LWMTX] tid %lu acquired slot %u\n", GetCurrentThreadId(), slot);
    }
#else
    pthread_mutex_lock(&s_lwmutex[slot].mtx);
#endif

    lwmutex->lock_var = 1;
    lwmutex->recursive_count++;
    return CELL_OK;
}

s32 sys_lwmutex_trylock(sys_lwmutex_t_hle* lwmutex)
{
    YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&s_lwmutex[slot].cs))
        return CELL_EBUSY;
#else
    if (pthread_mutex_trylock(&s_lwmutex[slot].mtx) != 0)
        return CELL_EBUSY;
#endif

    lwmutex->lock_var = 1;
    lwmutex->recursive_count++;
    return CELL_OK;
}

s32 sys_lwmutex_unlock(sys_lwmutex_t_hle* lwmutex)
{
    YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;

    lwmutex->recursive_count--;
    if (lwmutex->recursive_count == 0)
        lwmutex->lock_var = 0;

#ifdef _WIN32
    if (lw_global()) { LeaveCriticalSection(&s_lw_global); return CELL_OK; }
    LeaveCriticalSection(&s_lwmutex[slot].cs);
#else
    pthread_mutex_unlock(&s_lwmutex[slot].mtx);
#endif
    return CELL_OK;
}

s32 sys_lwmutex_destroy(sys_lwmutex_t_hle* lwmutex)
{
    YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
#ifdef _WIN32
    printf("[sysPrxForUser] sys_lwmutex_destroy(guest=0x%08X) [host tid %lu]\n",
           lwmutex ? YZ_GUEST_ADDR(lwmutex) : 0, GetCurrentThreadId());
#else
    printf("[sysPrxForUser] sys_lwmutex_destroy(guest=0x%08X)\n",
           lwmutex ? YZ_GUEST_ADDR(lwmutex) : 0);
#endif

    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use) {
        printf("[sysPrxForUser] sys_lwmutex_destroy -> ESRCH\n");
        return CELL_ESRCH;   /* already destroyed / never created */
    }

    /* lv2 refuses to destroy a held lwmutex (CELL_EBUSY) - games rely on
     * this when tearing down a heap another thread is still allocating
     * from: the EBUSY keeps the lock (and the heap) alive. */
#ifdef _WIN32
    if (!TryEnterCriticalSection(&s_lwmutex[slot].cs)) {
        printf("[sysPrxForUser] sys_lwmutex_destroy -> EBUSY\n");
        return CELL_EBUSY;
    }
    LeaveCriticalSection(&s_lwmutex[slot].cs);
    DeleteCriticalSection(&s_lwmutex[slot].cs);
#else
    if (pthread_mutex_trylock(&s_lwmutex[slot].mtx) != 0)
        return CELL_EBUSY;
    pthread_mutex_unlock(&s_lwmutex[slot].mtx);
    pthread_mutex_destroy(&s_lwmutex[slot].mtx);
#endif
    s_lwmutex[slot].in_use = 0;

    memset(lwmutex, 0, sizeof(*lwmutex));
    printf("[sysPrxForUser] sys_lwmutex_destroy -> OK\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Lightweight condition variable
 * -----------------------------------------------------------------------*/

s32 sys_lwcond_create(sys_lwcond_t_hle* lwcond, sys_lwmutex_t_hle* lwmutex,
                      const sys_lwcond_attribute_t* attr)
{
    YZ_XLAT(lwcond, sys_lwcond_t_hle*);
    YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
    YZ_XLAT(attr, const sys_lwcond_attribute_t*);
    printf("[sysPrxForUser] sys_lwcond_create(name='%.8s')\n",
           attr ? attr->name : "???");

    if (!lwcond || !lwmutex)
        return CELL_EFAULT;

    slot_lock();
    u32 idx = s_lwcond_next;
    for (u32 i = 0; i < MAX_LWCOND; i++) {
        u32 slot = (idx + i) % MAX_LWCOND;
        if (!s_lwcond[slot].in_use) {
            LwCondSlot* c = &s_lwcond[slot];
            c->in_use = 1;
            c->lwmutex_id = lwmutex->sleep_queue - 1;

#ifdef _WIN32
            InitializeConditionVariable(&c->cv);
#else
            pthread_cond_init(&c->cv, NULL);
#endif

            lwcond->lwcond_queue = slot + 1;
            s_lwcond_next = (slot + 1) % MAX_LWCOND;
            slot_unlock();
            return CELL_OK;
        }
    }
    slot_unlock();
    return CELL_EAGAIN;
}

s32 sys_lwcond_signal(sys_lwcond_t_hle* lwcond)
{
    YZ_XLAT(lwcond, sys_lwcond_t_hle*);
    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot >= MAX_LWCOND || !s_lwcond[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    WakeConditionVariable(&s_lwcond[slot].cv);
#else
    pthread_cond_signal(&s_lwcond[slot].cv);
#endif
    return CELL_OK;
}

s32 sys_lwcond_signal_all(sys_lwcond_t_hle* lwcond)
{
    YZ_XLAT(lwcond, sys_lwcond_t_hle*);
    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot >= MAX_LWCOND || !s_lwcond[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    WakeAllConditionVariable(&s_lwcond[slot].cv);
#else
    pthread_cond_broadcast(&s_lwcond[slot].cv);
#endif
    return CELL_OK;
}

s32 sys_lwcond_wait(sys_lwcond_t_hle* lwcond, u64 timeout)
{
    YZ_XLAT(lwcond, sys_lwcond_t_hle*);
    fprintf(stderr, "[WAIT] lwcond_wait(timeout=%llu)\n", (unsigned long long)timeout);
    if (!lwcond) return CELL_EFAULT;

    u32 cslot = lwcond->lwcond_queue - 1;
    if (cslot >= MAX_LWCOND || !s_lwcond[cslot].in_use)
        return CELL_ESRCH;

    u32 mslot = s_lwcond[cslot].lwmutex_id;
    if (mslot >= MAX_LWMUTEX || !s_lwmutex[mslot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    DWORD ms = (timeout == 0) ? INFINITE : (DWORD)(timeout / 1000);
    if (!SleepConditionVariableCS(&s_lwcond[cslot].cv,
                                   &s_lwmutex[mslot].cs, ms))
    {
        if (GetLastError() == ERROR_TIMEOUT)
            return CELL_ETIMEDOUT;
        return CELL_EFAULT;
    }
#else
    if (timeout == 0) {
        pthread_cond_wait(&s_lwcond[cslot].cv, &s_lwmutex[mslot].mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout / 1000000);
        ts.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&s_lwcond[cslot].cv,
                                         &s_lwmutex[mslot].mtx, &ts);
        if (rc == 110 /* ETIMEDOUT */)
            return CELL_ETIMEDOUT;
    }
#endif

    return CELL_OK;
}

s32 sys_lwcond_destroy(sys_lwcond_t_hle* lwcond)
{
    YZ_XLAT(lwcond, sys_lwcond_t_hle*);
    printf("[sysPrxForUser] sys_lwcond_destroy()\n");

    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot < MAX_LWCOND && s_lwcond[slot].in_use) {
#ifndef _WIN32
        pthread_cond_destroy(&s_lwcond[slot].cv);
#endif
        s_lwcond[slot].in_use = 0;
    }

    memset(lwcond, 0, sizeof(*lwcond));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Heap management (wraps malloc)
 * -----------------------------------------------------------------------*/

s32 sys_heap_create_heap(sys_heap_t* heap, u32 start_addr, u32 size,
                          u32 flags, void* alloc_func, void* free_func)
{
    (void)start_addr; (void)size; (void)flags;
    (void)alloc_func; (void)free_func;

    printf("[sysPrxForUser] sys_heap_create_heap(size=%u)\n", size);

    if (!heap) return CELL_EFAULT;

    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!s_heaps[i].in_use) {
            s_heaps[i].in_use = 1;
            s_heaps[i].id = s_next_heap_id++;
            vm_write32((u32)(uintptr_t)heap, s_heaps[i].id);
            return CELL_OK;
        }
    }
    return CELL_ENOMEM;
}

s32 sys_heap_destroy_heap(sys_heap_t heap)
{
    printf("[sysPrxForUser] sys_heap_destroy_heap(id=%u)\n", heap);

    for (int i = 0; i < MAX_HEAPS; i++) {
        if (s_heaps[i].in_use && s_heaps[i].id == heap) {
            s_heaps[i].in_use = 0;
            return CELL_OK;
        }
    }
    return CELL_ESRCH;
}

/* Guest-address bump allocator for the _sys_heap_* API. These return a GUEST
 * effective address (the caller writes to it through the VM), so a host malloc
 * pointer is wrong -- the guest would treat it as a 32-bit EA and fault. Hand
 * out addresses from a dedicated guest window above the sys_memory window. */
#define YZ_HEAP_BASE 0x50000000u
#define YZ_HEAP_END  0x58000000u
static u32 s_heap_bump = 0;

static u32 yz_heap_alloc(u32 size, u32 align)
{
    if (s_heap_bump == 0) s_heap_bump = YZ_HEAP_BASE;
    if (align < 16) align = 16;
    s_heap_bump = (s_heap_bump + align - 1) & ~(align - 1);
    u32 ea = s_heap_bump;
    s_heap_bump += (size + 15) & ~15u;
    if (s_heap_bump > YZ_HEAP_END) return 0;
    return ea;   /* guest EA; flat VM commits pages on first access */
}

void* sys_heap_malloc(sys_heap_t heap, u32 size)
{
    (void)heap;
    return (void*)(uintptr_t)yz_heap_alloc(size, 16);
}

s32 sys_heap_free(sys_heap_t heap, void* ptr)
{
    (void)heap; (void)ptr;   /* bump allocator: no per-alloc free */
    return CELL_OK;
}

void* sys_heap_memalign(sys_heap_t heap, u32 align, u32 size)
{
    (void)heap;
    return (void*)(uintptr_t)yz_heap_alloc(size, align);
#if 0
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void* ptr = NULL;
    if (posix_memalign(&ptr, align, size) != 0)
        return NULL;
    return ptr;
#endif
#endif
}

/* _sys_heap_create_heap (libdbgfont / dinkum ABI): RETURNS a non-zero heap id
 * in r3 (args name/type/blocksize/flags in r3..r6). Distinct from the out-param
 * sys_heap_create_heap above; _sys_heap_malloc bump-allocs regardless of id. */
u32 sys_heap_create_heap_ret(u32 name, u32 type, u32 blocksize, u32 flags)
{
    (void)name; (void)type; (void)blocksize; (void)flags;
    static u32 s_hid = 0x100;
    return s_hid++;
}

/* ---------------------------------------------------------------------------
 * PRX utilities
 * -----------------------------------------------------------------------*/

s32 sys_prx_exitspawn_with_level(void)
{
    printf("[sysPrxForUser] sys_prx_exitspawn_with_level() - no-op\n");
    return CELL_OK;
}

s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id)
{
    (void)flags;
    /* name/id arrive as raw GUEST effective addresses (the generic HLE adapter
     * forwards gpr3.. untranslated). Translate before touching memory -- else
     * printf("%s", name) derefs a bare host address and AVs. This path first
     * appears once real libsre is loaded (its name string lives in the libsre
     * image at 0x3000xxxx). Same bug class as the _sys_memset fix above. */
    const char* hname = (const char*)yz_g2h(name);
    u32* hid = (u32*)yz_g2h(id);
    printf("[sysPrxForUser] sys_prx_get_module_id_by_name('%s')\n",
           hname ? hname : "(null)");

    if (!hid) return CELL_EFAULT;
    *hid = 0; /* fake module ID */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Random number generation
 *
 * Used by many games for seeding RNG, crypto operations, UUID generation.
 * We use the host OS PRNG for quality random data.
 * -----------------------------------------------------------------------*/

s32 sys_get_random_number(void* buf, u64 size)
{
    if (!buf || size == 0) return CELL_EFAULT;
    buf = yz_g2h(buf);   /* guest EA -> host pointer before the OS fills it */

#ifdef _WIN32
    /* Use BCryptGenRandom on Windows */
    #include <bcrypt.h>
    NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        /* Fallback: fill with simple pseudo-random */
        u8* p = (u8*)buf;
        for (u64 i = 0; i < size; i++)
            p[i] = (u8)(rand() & 0xFF);
    }
#else
    /* Use /dev/urandom on Unix */
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(buf, 1, (size_t)size, f);
        fclose(f);
    } else {
        u8* p = (u8*)buf;
        for (u64 i = 0; i < size; i++)
            p[i] = (u8)(rand() & 0xFF);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Console I/O (debug)
 * -----------------------------------------------------------------------*/

s32 console_putc(s32 ch)
{
    fputc(ch, stderr);
    return CELL_OK;
}

s32 console_getc(void)
{
    return -1; /* no input */
}

s32 console_write(const void* buf, u32 len)
{
    if (buf && len > 0)
        fwrite(buf, 1, len, stderr);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Process info
 * -----------------------------------------------------------------------*/

s32 sys_process_get_paramsfo(void* buf)
{
    /* PARAM.SFO data — return a minimal valid SFO with title ID */
    if (!buf) return CELL_EFAULT;
    memset(yz_g2h(buf), 0, 256);
    return CELL_OK;
}
