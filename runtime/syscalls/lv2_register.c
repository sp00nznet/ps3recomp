/*
 * ps3recomp - LV2 syscall registration
 *
 * Calls all sys_X_init functions to populate the syscall dispatch table
 * with real HLE handlers.
 *
 * Registration order matters for conflicting syscall numbers:
 * timers are registered before events, so event handlers take precedence
 * for the colliding numbers (141, 142, 145). Timer sleep/time functions
 * remain available as direct C calls for the runtime to use.
 */

#include <stdlib.h>   /* calloc, free */
#include "lv2_syscall_table.h"
#include "sys_ppu_thread.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_semaphore.h"
#include "sys_rwlock.h"
#include "sys_event.h"
#include "sys_timer.h"
#include "sys_memory.h"
#include "sys_vm.h"
#include "sys_fs.h"
#include "ps3emu/spu_fallback.h"
#include "sys_event.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * TTY syscalls (used by PS3 CRT for debug output)
 *
 * sys_tty_read  (402) — read from TTY (stdin)
 * sys_tty_write (403) — write to TTY (stdout/stderr)
 *
 * These are among the most commonly called syscalls in CRT startup.
 * -----------------------------------------------------------------------*/

extern uint8_t* vm_base;

static int64_t sys_tty_write(ppu_context* ctx)
{
    /* s32 sys_tty_write(s32 ch, const void* buf, u32 len, u32* pwritelen) */
    uint32_t ch     = (uint32_t)ctx->gpr[3];
    uint32_t buf_ea = (uint32_t)ctx->gpr[4];
    uint32_t len    = (uint32_t)ctx->gpr[5];
    uint32_t pwr_ea = (uint32_t)ctx->gpr[6];

    (void)ch; /* channel number, ignored */

    if (buf_ea && len > 0 && vm_base) {
        /* Write guest string data to host stderr */
        fwrite(vm_base + buf_ea, 1, len, stderr);
        fflush(stderr);
        /* DIAGNOSTIC (FLOW_PSSGTRACE=1): when the title logs a PhyreEngine
         * init failure, dump the guest back-chain so we can locate the failing
         * function (the message itself goes through here, not _sys_printf). */
        if (getenv("FLOW_PSSGTRACE") && len < 4096) {
            char tmp[256]; uint32_t n = len < 255 ? len : 255;
            memcpy(tmp, vm_base + buf_ea, n); tmp[n] = 0;
            /* The PhyreEngine failure message is written in fragments, so no
             * single buffer holds "PSSG". Dump the back-chain for any fragment
             * carrying init/error/Phyre keywords. */
            if (strstr(tmp, "PSSG") || strstr(tmp, "Init") || strstr(tmp, "App") ||
                strstr(tmp, "fail") || strstr(tmp, "Error") || strstr(tmp, "rror") ||
                strstr(tmp, "PSpu") || strstr(tmp, "ation")) {
                uint32_t sp = (uint32_t)ctx->gpr[1];
                fprintf(stderr, "[pssg-bt] tty_write \"%.50s\" lr=0x%08X\n", tmp, (uint32_t)ctx->lr);
                for (int i = 0; i < 28 && sp && sp < 0x10000000u; i++) {
                    uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                    nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                    if (nsp <= sp || nsp >= 0x10000000u) break;
                    uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                    lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                    fprintf(stderr, "[pssg-bt]   #%d lr=0x%08X\n", i, lr);
                    sp = nsp;
                }
                fflush(stderr);
            }
        }
    }

    /* Write back the number of bytes written */
    if (pwr_ea && vm_base) {
        uint32_t be_len = ((len >> 24) & 0xFF) | ((len >> 8) & 0xFF00) |
                          ((len << 8) & 0xFF0000) | ((len << 24) & 0xFF000000);
        memcpy(vm_base + pwr_ea, &be_len, 4);
    }

    return 0; /* CELL_OK */
}

static int64_t sys_tty_read(ppu_context* ctx)
{
    /* s32 sys_tty_read(s32 ch, void* buf, u32 len, u32* preadlen) */
    uint32_t prd_ea = (uint32_t)ctx->gpr[6];

    /* No TTY input available — return 0 bytes read */
    if (prd_ea && vm_base)
        memset(vm_base + prd_ea, 0, 4);

    return 0;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/

#define SYS_TTY_READ   402
#define SYS_TTY_WRITE  403

/* ---------------------------------------------------------------------------
 * Stateful SPU thread group tracker
 *
 * We don't execute SPU programs — SPURS job queues, SPU tasks, and raw SPU
 * threads all resolve to an empty "thread completed normally" result. But
 * the PPU-side wrappers (PhyreEngine's SPURS wrapper in particular) check
 * returned IDs, out-param cause/status fields, and per-thread exit codes
 * after every call. A flat "return 0" stub leaves the out-params as heap
 * garbage and the wrapper then throws a C++ exception.
 *
 * This tracker assigns monotonically-increasing IDs, walks a small state
 * machine, and writes all the out-params each syscall is documented to
 * set. It doesn't try to emulate actual SPU work — the group transitions
 * straight from STARTED to STOPPED with exit code 0.
 *
 * Cause values match the public Sony SDK headers:
 *   GROUP_EXIT       = 0x0001 — sys_spu_thread_group_exit() was called
 *   ALL_THREADS_EXIT = 0x0002 — all threads completed their entry fn
 *   TERMINATED       = 0x0004 — sys_spu_thread_group_terminate() fired
 * -----------------------------------------------------------------------*/

#define SPU_GROUP_STATE_INITIALIZED  0
#define SPU_GROUP_STATE_READY        1
#define SPU_GROUP_STATE_RUNNING      2
#define SPU_GROUP_STATE_STOPPED      3
#define SPU_GROUP_STATE_DESTROYED    4

#define SPU_GROUP_CAUSE_GROUP_EXIT        0x0001u
#define SPU_GROUP_CAUSE_ALL_THREADS_EXIT  0x0002u
#define SPU_GROUP_CAUSE_TERMINATED        0x0004u

#define MAX_SPU_GROUPS   32
#define MAX_SPU_THREADS  (MAX_SPU_GROUPS * 8)

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE spu_thread_handle_t;
typedef HANDLE spu_thread_event_t;
#else
#  include <pthread.h>
typedef pthread_t spu_thread_handle_t;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             done;
} spu_thread_event_t;
#endif

typedef struct {
    int      in_use;
    uint32_t tid;            /* thread id (unique across all groups) */
    uint32_t group_id;       /* parent group */
    uint32_t index;          /* slot within the group */
    int32_t  exit_status;
    uint32_t entry_point;    /* initial SPU image entry (informational) */
    /* Args block passed via sys_spu_thread_initialize (.args_ea) and
     * sys_spu_thread_set_argument (per-thread). For SPURS this holds the
     * 4 register-style args (arg1..arg4) packed into a guest struct.
     * The PPU fallback receives args_ea so it can decode whatever format
     * the registered job expects. */
    uint32_t args_ea;
    uint32_t args_size;
    /* Async fallback execution. host_thread is set when group_start spawned
     * a host thread for this SPU thread's PPU fallback; finish_event is
     * signalled when the handler returns; running indicates the thread is
     * still in flight. group_join waits on finish_event for each running
     * thread. */
    spu_thread_handle_t host_thread;
    spu_thread_event_t  finish_event;
    int                 running;
    spu_ppu_fallback_fn fb_handler;
    void*               fb_user;
    /* Virtual local store. Real SPU has 256 KB. Allocated lazily on first
     * sys_spu_thread_write_ls / read_ls. PPU fallbacks can also reach this
     * via the public spu_thread_get_local_store() helper, simulating the
     * common pattern where the PPU writes job state into LS, the SPU runs
     * and writes results back to LS, then PPU reads them. */
    uint8_t*            local_store;
    /* sys_spu_thread_connect_event(thread, eq, et): binds this SPU thread's
     * outbound interrupt-mailbox events to a PPU event queue. When the SPU
     * writes WrOutIntrMbox (or stop-and-signals), the runtime delivers an event
     * to connected_queue so PPU code blocked in sys_event_queue_receive wakes. */
    uint32_t            connected_queue;
    uint32_t            connect_spup;
} spu_thread_t;

typedef struct {
    int      in_use;
    uint32_t id;
    int      state;
    uint32_t num_threads;
    uint32_t thread_indices[8];  /* table index into s_spu_threads */
    char     name[32];
    int32_t  exit_status;        /* final ppu-side status the group reports */
    uint32_t cause;              /* how the group ended */
    /* Event queue connected via sys_spu_thread_group_connect_event[_all_threads].
     * When the group transitions to STOPPED (in group_join), an event is pushed
     * into this queue with source = SYS_SPU_THREAD_GROUP_EVENT (0x100..) so
     * PPU code blocked on sys_event_queue_receive wakes up. */
    uint32_t event_queue_id;
} spu_group_t;

static spu_group_t  s_spu_groups[MAX_SPU_GROUPS];
static spu_thread_t s_spu_threads[MAX_SPU_THREADS];
static uint32_t     s_spu_next_group_id  = 0x1000;
static uint32_t     s_spu_next_thread_id = 0x2000;
static int          s_spu_initialized    = 0;

static spu_group_t* spu_find_group(uint32_t id)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (s_spu_groups[i].in_use && s_spu_groups[i].id == id)
            return &s_spu_groups[i];
    }
    return NULL;
}

static spu_group_t* spu_alloc_group(void)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (!s_spu_groups[i].in_use) {
            memset(&s_spu_groups[i], 0, sizeof(s_spu_groups[i]));
            s_spu_groups[i].in_use = 1;
            s_spu_groups[i].id     = s_spu_next_group_id++;
            s_spu_groups[i].state  = SPU_GROUP_STATE_INITIALIZED;
            s_spu_groups[i].exit_status = 0;
            s_spu_groups[i].cause  = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
            return &s_spu_groups[i];
        }
    }
    return NULL;
}

static spu_thread_t* spu_find_thread(uint32_t tid)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (s_spu_threads[i].in_use && s_spu_threads[i].tid == tid)
            return &s_spu_threads[i];
    }
    return NULL;
}

static spu_thread_t* spu_alloc_thread(void)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (!s_spu_threads[i].in_use) {
            memset(&s_spu_threads[i], 0, sizeof(s_spu_threads[i]));
            s_spu_threads[i].in_use = 1;
            s_spu_threads[i].tid    = s_spu_next_thread_id++;
            return &s_spu_threads[i];
        }
    }
    return NULL;
}

static void vm_write_be32(uint32_t guest_addr, uint32_t val)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return;
    uint8_t* p = vm_base + guest_addr;
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >>  8);
    p[3] = (uint8_t)(val);
}

static uint32_t vm_read_be32(uint32_t guest_addr)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return 0;
    const uint8_t* p = vm_base + guest_addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

/* sys_spu_initialize(nspu, nrawspu) — one-shot global init */
static int64_t sys_spu_initialize_handler(ppu_context* ctx)
{
    uint32_t nspu    = (uint32_t)ctx->gpr[3];
    uint32_t nrawspu = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[SPU] initialize(nspu=%u, nrawspu=%u)\n", nspu, nrawspu);
    fflush(stderr);
    s_spu_initialized = 1;
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_create(out_id_ea, num, name_ea, attr_ea) */
static int64_t sys_spu_thread_group_create_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t out_ea   = (uint32_t)ctx->gpr[3];
    uint32_t num      = (uint32_t)ctx->gpr[4];
    uint32_t name_ea  = (uint32_t)ctx->gpr[5];
    fprintf(stderr, "[SPU] thread_group_create(num=%u)\n", num);
    uint32_t attr_ea  = (uint32_t)ctx->gpr[6];

    spu_group_t* g = spu_alloc_group();
    if (!g) {
        fprintf(stderr, "[SPU] group_create: out of groups\n");
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)-1; /* EAGAIN-ish */
        return -1;
    }
    if (num > 8) num = 8;
    g->num_threads = num;

    if (name_ea && vm_base) {
        const char* src = (const char*)(vm_base + name_ea);
        size_t i = 0;
        for (; i < sizeof(g->name) - 1 && src[i]; i++)
            g->name[i] = src[i];
        g->name[i] = 0;
    }

    vm_write_be32(out_ea, g->id);

    fprintf(stderr, "[SPU] group_create -> id=0x%X num=%u name=%.31s attr=0x%08X\n",
            g->id, num, g->name, attr_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* HLE SPURS kernel (YDKJ_SPURSKERNEL): libsre hands its 5 cellSpurs SPU threads
 * an EMPTY image (the firmware SPU kernel can't run as static-recompiled code),
 * so group_start would instantly complete them and the SPURS handler asserts the
 * SPU side is dead. Instead, register THIS as the threads' PPU fallback: it runs
 * as a host "SPU" that keeps the group genuinely RUNNING. Minimal first version
 * idles; the full version polls the SPURS taskset (ctx = args_ea) and dispatches
 * the title's lifted SPU task images. */
#define YDKJ_SPURS_KERNEL_ENTRY 0x5B555253u  /* 'SURS' marker entry */
/* Project-side runner: runs the REAL lifted SPURS SPU kernel (sk_a, lifted in
 * the title build, not the runtime lib) on this SPU thread. Set by the project
 * at startup (src/ydkj_spurs_kernel.c). If unset, fall back to the idle loop. */
int32_t (*g_ydkj_spurs_kernel_run)(uint32_t tid, uint32_t args_ea) = 0;
static int32_t ydkj_hle_spurs_kernel(uint32_t tid, uint32_t args_ea,
                                     uint32_t args_size, void* user)
{
    (void)args_size; (void)user;
    fprintf(stderr, "[HLE-SPURS] kernel SPU tid=0x%X ctx=0x%08X running\n", tid, args_ea);
    fflush(stderr);
    if (g_ydkj_spurs_kernel_run)
        return g_ydkj_spurs_kernel_run(tid, args_ea);   /* run the lifted kernel */
    /* Keep the group running so the SPURS handler sees a live SPU. */
    for (int i = 0; i < 1200; i++) {
#ifdef _WIN32
        Sleep(50);
#else
        struct timespec ts = {0, 50*1000*1000}; nanosleep(&ts, 0);
#endif
    }
    return 0;
}

/* sys_spu_thread_initialize(out_tid_ea, group_id, thread_num, img_ea, attr_ea, args_ea) */
static int64_t sys_spu_thread_initialize_handler(ppu_context* ctx)
{
    uint32_t out_tid_ea = (uint32_t)ctx->gpr[3];
    uint32_t group_id   = (uint32_t)ctx->gpr[4];
    uint32_t thread_num = (uint32_t)ctx->gpr[5];
    uint32_t img_ea     = (uint32_t)ctx->gpr[6];
    /* attr_ea         = (uint32_t)ctx->gpr[7];  // unused */
    uint32_t args_ea    = (uint32_t)ctx->gpr[8];

    spu_group_t* g = spu_find_group(group_id);
    if (!g) {
        fprintf(stderr, "[SPU] thread_init: group 0x%X not found\n", group_id);
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)-1;
        return -1;
    }
    spu_thread_t* t = spu_alloc_thread();
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)-1;
        return -1;
    }
    t->group_id = group_id;
    t->index    = thread_num;
    /* Record the SPURS kernel context EA (the SPU thread's argument) so the
     * event layer can dispatch the title's real lifted SPU task runtime against
     * it when a PPU thread blocks waiting for SPU completion. */
    { extern uint32_t g_ydkj_spurs_ctx_ea; if (args_ea) g_ydkj_spurs_ctx_ea = args_ea; }
    /* Optional title hook: run the real lifted SPURS kernel on this SPU thread's
     * context (libsre leaves the kernel image empty + never starts the group). */
    { extern void (*g_spurs_kernel_hook)(uint32_t); if (g_spurs_kernel_hook) g_spurs_kernel_hook(args_ea); }
    /* Read entry point from the SPU image struct if available.
     * sys_spu_image layout: type/entry/segs/nsegs — entry at +4. */
    if (img_ea) t->entry_point = vm_read_be32(img_ea + 4);
    t->args_ea   = args_ea;
    t->args_size = 0;  /* not known until decoder reads it; sys_spu_thread_args is 32 B */

    /* Empty image (entry=0) on a cellSpurs SPU thread -> route to the HLE SPURS
     * kernel (YDKJ_SPURSKERNEL) so group_start runs a live SPU instead of an
     * instant no-op. */
    if (t->entry_point == 0 && getenv("YDKJ_SPURSKERNEL")) {
        t->entry_point = YDKJ_SPURS_KERNEL_ENTRY;
        static int s_reg = 0;
        if (!s_reg) { s_reg = 1;
            spu_register_ppu_fallback(YDKJ_SPURS_KERNEL_ENTRY, ydkj_hle_spurs_kernel, 0); }
    }

    if (getenv("YDKJ_SPUIMG") && img_ea && thread_num == 0) {
        uint32_t type  = vm_read_be32(img_ea + 0);
        uint32_t entry = vm_read_be32(img_ea + 4);
        uint32_t segs  = vm_read_be32(img_ea + 8);
        uint32_t nsegs = vm_read_be32(img_ea + 12);
        fprintf(stderr, "[SPUIMG] img=0x%08X type=0x%X entry=0x%X segs=0x%08X nsegs=%u\n",
                img_ea, type, entry, segs, nsegs);
        for (uint32_t s = 0; s < nsegs && s < 8; s++) {
            uint32_t b = segs + s * 0x18;  /* sys_spu_segment: type,ls,size,src(pa64) */
            fprintf(stderr, "[SPUIMG]  seg%u type=0x%X ls=0x%X size=0x%X src=0x%08X%08X\n",
                    s, vm_read_be32(b+0), vm_read_be32(b+4), vm_read_be32(b+8),
                    vm_read_be32(b+0x10), vm_read_be32(b+0x14));
        }
        fflush(stderr);
    }

    if (thread_num < 8)
        g->thread_indices[thread_num] = (uint32_t)(t - s_spu_threads);

    /* EXPERIMENT (YDKJ_SPUREADY): the lifted libsre cellSpursInitialize busy-polls the
     * SPU-thread descriptor field at img_ea+0x38 (e.g. 0x101671A8) for a non-zero
     * "thread created/ready" status BEFORE it populates the SPURS instance / starts the
     * group — but nothing in our HLE ever writes it (real lv2 does). Write the tid there
     * to clear the poll so libsre can proceed past init. Diagnostic; value may need tuning. */
    if (img_ea && getenv("YDKJ_SPUREADY")) {
        uint32_t before = vm_read_be32(img_ea + 0x38);
        vm_write_be32(img_ea + 0x38, t->tid);
        fprintf(stderr, "[SPUREADY] wrote tid=0x%X to img+0x38=0x%08X (was 0x%08X)\n",
                t->tid, img_ea + 0x38, before);
        fflush(stderr);
    }

    vm_write_be32(out_tid_ea, t->tid);

    fprintf(stderr, "[SPU] thread_init group=0x%X index=%u img=0x%08X args=0x%08X -> tid=0x%X entry=0x%08X\n",
            group_id, thread_num, img_ea, args_ea, t->tid, t->entry_point);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* Host-thread entry for a PPU-fallback SPU thread. */
#ifdef _WIN32
static DWORD WINAPI spu_fallback_thread_proc(LPVOID arg)
#else
static void* spu_fallback_thread_proc(void* arg)
#endif
{
    spu_thread_t* t = (spu_thread_t*)arg;
    int32_t rc = 0;
    if (t->fb_handler) {
        rc = t->fb_handler(t->tid, t->args_ea, t->args_size, t->fb_user);
    }
    t->exit_status = rc;
    /* Mark complete and signal anyone waiting in group_join. */
#ifdef _WIN32
    t->running = 0;
    SetEvent(t->finish_event);
    return 0;
#else
    pthread_mutex_lock(&t->finish_event.mu);
    t->running = 0;
    t->finish_event.done = 1;
    pthread_cond_broadcast(&t->finish_event.cv);
    pthread_mutex_unlock(&t->finish_event.mu);
    return NULL;
#endif
}

/* sys_spu_thread_group_start(id) */
static int64_t sys_spu_thread_group_start_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (!g) { ctx->gpr[3] = (uint64_t)(int64_t)-1; return -1; }
    g->state = SPU_GROUP_STATE_RUNNING;

    /* DIAG: dump the CellSpurs instance @0x40009D00 at group_start time, to see
     * whether libsre has populated it BEFORE the SPU kernel threads spawn. */
    { extern uint8_t* vm_base; static int s_d=0;
      if (vm_base && s_d++ < 4) {
        const uint8_t* in = vm_base + 0x40009D00;
        fprintf(stderr, "[INSTDUMP] group_start id=0x%X CellSpurs@0x40009D00 (0x140 bytes):\n", id);
        for (int row=0; row<10; row++){
            fprintf(stderr, "  +0x%03X:", row*16);
            for (int i=0;i<4;i++){ int o=row*16+i*4; uint32_t w=((uint32_t)in[o]<<24)|((uint32_t)in[o+1]<<16)|((uint32_t)in[o+2]<<8)|in[o+3]; fprintf(stderr," %08X",w);}
            fprintf(stderr, "\n");
        }
        fflush(stderr);
        /* Arm a page-guard on the instance page so we catch the libsre function
         * that writes the CellSpurs struct (WWATCH misses memcpy/DMA writes). */
        if (getenv("YDKJ_GUARD_INST")) { extern void ppu_guard_page(uint32_t); ppu_guard_page(0x40009D00); }
      } }

    /* For each thread in the group, look up a registered PPU fallback by
     * the thread's SPU image entry point. Threads with a fallback run on
     * a host thread (real concurrency, like real SPUs). Threads without
     * a fallback complete instantly with status 0.
     * group_join() blocks until all spawned host threads finish. */
    int spawned = 0;
    int instant = 0;
    for (uint32_t i = 0; i < g->num_threads && i < 8; i++) {
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!t->in_use) continue;
        void* user = NULL;
        spu_ppu_fallback_fn fb = spu_lookup_ppu_fallback(t->entry_point, &user);
        if (!fb) {
            t->exit_status = 0;
            t->running = 0;
            instant++;
            continue;
        }
        t->fb_handler = fb;
        t->fb_user    = user;
        t->running    = 1;
#ifdef _WIN32
        /* Manual-reset event so multiple group_join callers all see "set" */
        if (!t->finish_event)
            t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        else
            ResetEvent(t->finish_event);
        /* Lifted SPU loops can become deep C tail-call recursion; give SPU host
         * threads a large stack (reserved). Bumped to 512 MB to diagnose whether
         * the cri/taskset-policy dispatch chain overflows (env YDKJ_BIGSTACK). */
        SIZE_T _stk = getenv("YDKJ_BIGSTACK") ? (SIZE_T)512 * 1024 * 1024
                                              : (SIZE_T)16 * 1024 * 1024;
        t->host_thread = CreateThread(NULL, _stk,
                                      spu_fallback_thread_proc, t,
                                      STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#else
        pthread_mutex_init(&t->finish_event.mu, NULL);
        pthread_cond_init(&t->finish_event.cv, NULL);
        t->finish_event.done = 0;
        pthread_create(&t->host_thread, NULL, spu_fallback_thread_proc, t);
#endif
        fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X args=0x%08X -> spawned host thread\n",
                id, t->tid, t->entry_point, t->args_ea);
        spawned++;
    }

    if (spawned == 0) {
        g->state = SPU_GROUP_STATE_STOPPED;
        g->cause = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
        g->exit_status = 0;
        fprintf(stderr, "[SPU] group_start id=0x%X (no fallback for any of %u thread(s); instantly completed)\n",
                id, g->num_threads);
    } else {
        fprintf(stderr, "[SPU] group_start id=0x%X (%d host threads running, %d instant)\n",
                id, spawned, instant);
    }
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_join(id, *cause, *status) */
static int64_t sys_spu_thread_group_join_handler(ppu_context* ctx)
{
    uint32_t id         = (uint32_t)ctx->gpr[3];
    uint32_t cause_ea   = (uint32_t)ctx->gpr[4];
    uint32_t status_ea  = (uint32_t)ctx->gpr[5];

    spu_group_t* g = spu_find_group(id);
    if (!g) {
        /* Unknown group id — Sony returns CELL_ESRCH but we've seen
         * games probe with stale IDs, so be lenient and fake a success. */
        vm_write_be32(cause_ea,  SPU_GROUP_CAUSE_ALL_THREADS_EXIT);
        vm_write_be32(status_ea, 0);
        fprintf(stderr, "[SPU] group_join id=0x%X (unknown, faked ok)\n", id);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }
    /* If the group was never started, mark it stopped so a subsequent
     * destroy doesn't trip a "still running" check. */
    if (g->state == SPU_GROUP_STATE_INITIALIZED ||
        g->state == SPU_GROUP_STATE_READY) {
        g->state = SPU_GROUP_STATE_STOPPED;
    }

    /* Wait for any host-thread fallbacks to finish, then collect the
     * worst exit status. Real SPU group_join is a blocking syscall —
     * games rely on it to know all SPU work is done before reading
     * back results. */
    if (g->state == SPU_GROUP_STATE_RUNNING) {
        int32_t worst = 0;
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx >= MAX_SPU_THREADS) continue;
            spu_thread_t* t = &s_spu_threads[idx];
            if (!t->in_use) continue;
            if (t->running) {
#ifdef _WIN32
                if (t->finish_event)
                    WaitForSingleObject(t->finish_event, INFINITE);
                if (t->host_thread) {
                    CloseHandle(t->host_thread);
                    t->host_thread = NULL;
                }
#else
                pthread_mutex_lock(&t->finish_event.mu);
                while (!t->finish_event.done)
                    pthread_cond_wait(&t->finish_event.cv, &t->finish_event.mu);
                pthread_mutex_unlock(&t->finish_event.mu);
                pthread_join(t->host_thread, NULL);
                pthread_mutex_destroy(&t->finish_event.mu);
                pthread_cond_destroy(&t->finish_event.cv);
#endif
            }
            if (t->exit_status < worst) worst = t->exit_status;
        }
        g->exit_status = worst;
        g->cause       = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
        g->state       = SPU_GROUP_STATE_STOPPED;
    }

    /* Notify any connected event queue. Real PS3 sends a SYS_SPU_THREAD_GROUP
     * event with type-specific data; we collapse to a "group stopped" tag
     * (data1 = group_id, data2 = exit_status, data3 = cause). PPU code
     * blocked in sys_event_queue_receive on this queue wakes up here. */
    if (g->event_queue_id) {
        sys_event_queue_push_by_id(g->event_queue_id,
                                   (uint64_t)g->id,
                                   (uint64_t)(int64_t)g->exit_status,
                                   (uint64_t)g->cause,
                                   0);
    }

    vm_write_be32(cause_ea,  g->cause);
    vm_write_be32(status_ea, (uint32_t)g->exit_status);

    fprintf(stderr, "[SPU] group_join id=0x%X cause=%u status=%d (event_queue=0x%X)\n",
            id, g->cause, g->exit_status, g->event_queue_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_destroy(id) */
static int64_t sys_spu_thread_group_destroy_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    /* EXPERIMENT (YDKJ_KEEPGROUP): libsre rolls back the SPURS kernel group during
     * cellSpursInitialize (the handler asserts the SPU side is dead). Skip the
     * destroy so the group + threads survive, to see whether libsre then proceeds
     * (group_start) or just re-asserts. Logs the caller for diagnosis. */
    if (getenv("YDKJ_KEEPGROUP") && id == 0x1000) {
        fprintf(stderr, "[SPU] group_destroy id=0x%X SKIPPED (YDKJ_KEEPGROUP) caller_lr=0x%08X cia=0x%08X\n",
                id, (uint32_t)ctx->lr, (uint32_t)ctx->cia);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }
    spu_group_t* g = spu_find_group(id);
    if (g) {
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx < MAX_SPU_THREADS) {
                spu_thread_t* t = &s_spu_threads[idx];
                if (t->local_store) {
                    free(t->local_store);
                    t->local_store = NULL;
                }
                t->in_use = 0;
            }
        }
        g->in_use = 0;
    }
    fprintf(stderr, "[SPU] group_destroy id=0x%X  caller_lr=0x%08X cia=0x%08X r3..r6=%08X %08X %08X %08X\n",
            id, (uint32_t)ctx->lr, (uint32_t)ctx->cia,
            (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
            (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_terminate(id, exit_status) */
static int64_t sys_spu_thread_group_terminate_handler(ppu_context* ctx)
{
    uint32_t id     = (uint32_t)ctx->gpr[3];
    int32_t  status = (int32_t)ctx->gpr[4];
    spu_group_t* g = spu_find_group(id);
    if (g) {
        g->state = SPU_GROUP_STATE_STOPPED;
        g->cause = SPU_GROUP_CAUSE_TERMINATED;
        g->exit_status = status;
    }
    fprintf(stderr, "[SPU] group_terminate id=0x%X status=%d\n", id, status);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_get_exit_status(tid, *status)
 * Real PS3: returns CELL_ESRCH for unknown tid, CELL_ESTAT if thread is
 * still running (caller should join the group first), otherwise 0 with
 * the exit code written through. */
static int64_t sys_spu_thread_get_exit_status_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t status_ea = (uint32_t)ctx->gpr[4];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (t->running) {
        /* Still in flight — Sony's behaviour. Games that want the exit code
         * synchronously should call group_join first. */
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010003; /* CELL_ESTAT */
        return -1;
    }
    vm_write_be32(status_ea, (uint32_t)t->exit_status);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_set_argument(tid, arg_ea) — doesn't affect us, log only */
static int64_t sys_spu_thread_set_argument_handler(ppu_context* ctx)
{
    uint32_t tid    = (uint32_t)ctx->gpr[3];
    uint32_t arg_ea = (uint32_t)ctx->gpr[4];

    /* Update the per-thread args pointer so any registered PPU fallback
     * picks it up at sys_spu_thread_group_start time. The shape of the
     * struct at arg_ea is whatever the game registered for — typically
     * a packed (arg1,arg2,arg3,arg4) tuple of 4 u64s on real SPUs. */
    spu_thread_t* t = spu_find_thread(tid);
    if (t) t->args_ea = arg_ea;

    fprintf(stderr, "[SPU] thread_set_argument tid=0x%X arg=0x%08X\n",
            tid, arg_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_connect_event(group_id, queue_id, event_type)
 * sys_spu_thread_group_connect_event_all_threads(group_id, queue_id, name, port)
 *
 * Both bind a SYS_EVENT queue to the group; we record queue_id so
 * group_join can push a completion event. Sony's docs distinguish event
 * types (group state changes vs SPU-emitted user events) but we collapse
 * them into "the queue gets notified when the group transitions to
 * STOPPED" — sufficient for the common SPURS pattern. */
static int64_t sys_spu_thread_group_connect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    uint32_t queue_id = (uint32_t)ctx->gpr[4];
    spu_group_t* g = spu_find_group(group_id);
    if (!g) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    g->event_queue_id = queue_id;
    fprintf(stderr, "[SPU] group_connect_event group=0x%X queue=0x%X\n",
            group_id, queue_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_disconnect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(group_id);
    if (g) g->event_queue_id = 0;
    fprintf(stderr, "[SPU] group_disconnect_event group=0x%X\n", group_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_connect_event(thread_id, eq_id, et) — bind an SPU thread's
 * interrupt events to a PPU event queue. Previously a no-op stub, so the SPU's
 * outbound interrupt mailbox had nowhere to deliver and PPU waiters on q=1/q=4
 * (cellSpurs SpursHdlr / AsyncLoad) blocked forever. Record the binding here;
 * the mailbox-delivery hook (below) uses it. */
static int64_t sys_spu_thread_connect_event_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t eq  = (uint32_t)ctx->gpr[4];
    uint32_t et  = (uint32_t)ctx->gpr[5];
    spu_thread_t* t = spu_find_thread(tid);
    if (t) { t->connected_queue = eq; t->connect_spup = et; }
    fprintf(stderr, "[SPU] thread_connect_event tid=0x%X queue=0x%X et=0x%X%s\n",
            tid, eq, et, t ? "" : " (thread not found)");
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* SPU -> PPU outbound mailbox delivery. Installed into spu_channels.c's
 * g_spu_out_mbox_hook; called when a (kernel/policy/task) SPU thread writes
 * WrOutMbox/WrOutIntrMbox. Route the value to the event queue bound to the SPU
 * thread (via connect_event) or its group, so a blocked PPU SpursHdlr/AsyncLoad
 * receive wakes. The event carries the mbox value in data1 so the handler can
 * dispatch on it. Only the interrupt mailbox (is_intr) raises a PPU event on
 * real hardware; the plain mailbox is PPU-polled, but we deliver both as events
 * here (harmless: a handler that doesn't expect data ignores it) gated so we
 * don't flood. */
extern int sys_event_queue_push_by_id(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void ydkj_spu_out_mbox_deliver(uint32_t group_id, uint32_t spu_id,
                                      int is_intr, uint32_t value)
{
    /* Find the queue: prefer the per-thread connect_event binding; fall back to
     * the group's connected queue. */
    uint32_t q = 0;
    spu_thread_t* t = spu_find_thread(spu_id);
    if (t && t->connected_queue) q = t->connected_queue;
    if (!q) { spu_group_t* g = spu_find_group(group_id); if (g) q = g->event_queue_id; }
    if (!q) return;
    /* SPURS SPU-event source convention: high word tags it as an SPU thread
     * event; data1 = the mailbox value. */
    sys_event_queue_push_by_id(q,
        ((uint64_t)spu_id << 32) | (is_intr ? 0x2u : 0x1u),
        (uint64_t)value, 0, 0);
    { static int s_w = 0; if (s_w++ < 32)
        fprintf(stderr, "[SPU->PPU] mbox deliver spu=0x%X intr=%d val=0x%08X -> q=%u\n",
                spu_id, is_intr, value, q); }
}

/* SPU virtual local store. Real hardware: 256 KB per SPU. We allocate on
 * first read/write so the common case (group with no LS access) doesn't
 * waste 256 KB × num_threads. */
#define SPU_LS_SIZE  (256 * 1024)
static uint8_t* spu_thread_get_or_alloc_ls(spu_thread_t* t)
{
    if (!t) return NULL;
    if (!t->local_store) {
        t->local_store = (uint8_t*)calloc(1, SPU_LS_SIZE);
    }
    return t->local_store;
}

/* sys_spu_thread_write_ls(tid, ls_offset, value, type)
 * Writes 1/2/4/8 bytes (per `type`: 1/2/4/8) into the SPU thread's LS
 * at ls_offset. Real PS3 sees this stored to the SPU's local memory; we
 * keep an independent per-thread buffer that the PPU and any registered
 * fallback can access via spu_thread_get_local_store(). */
static int64_t sys_spu_thread_write_ls_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t ls_offset = (uint32_t)ctx->gpr[4];
    uint64_t value     = (uint64_t)ctx->gpr[5];
    uint32_t type      = (uint32_t)ctx->gpr[6];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (ls_offset + type > SPU_LS_SIZE) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; /* CELL_EFAULT */
        return -1;
    }
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004; /* CELL_ENOMEM */
        return -1;
    }
    /* Big-endian store, mirroring guest convention. */
    switch (type) {
    case 1: ls[ls_offset] = (uint8_t)value; break;
    case 2: ls[ls_offset+0] = (uint8_t)(value >> 8);
            ls[ls_offset+1] = (uint8_t)value; break;
    case 4: for (int i = 0; i < 4; i++)
                ls[ls_offset+i] = (uint8_t)(value >> ((3-i)*8));
            break;
    case 8: for (int i = 0; i < 8; i++)
                ls[ls_offset+i] = (uint8_t)(value >> ((7-i)*8));
            break;
    default:
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_read_ls(tid, ls_offset, *value_out, type) */
static int64_t sys_spu_thread_read_ls_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t ls_offset = (uint32_t)ctx->gpr[4];
    uint32_t value_ea  = (uint32_t)ctx->gpr[5];
    uint32_t type      = (uint32_t)ctx->gpr[6];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t || !value_ea || !vm_base) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (ls_offset + type > SPU_LS_SIZE) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004;
        return -1;
    }
    /* Big-endian load → write to guest as 8 bytes (always); the syscall
     * is documented to write a u64 with the value zero-extended in the
     * high bits. */
    uint64_t value = 0;
    switch (type) {
    case 1: value = ls[ls_offset]; break;
    case 2: value = ((uint64_t)ls[ls_offset] << 8) | ls[ls_offset+1]; break;
    case 4:
        for (int i = 0; i < 4; i++)
            value = (value << 8) | ls[ls_offset+i];
        break;
    case 8:
        for (int i = 0; i < 8; i++)
            value = (value << 8) | ls[ls_offset+i];
        break;
    default:
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    /* Write 8-byte BE value to guest. */
    uint8_t* p = vm_base + value_ea;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(value >> ((7-i)*8));
    ctx->gpr[3] = 0;
    return 0;
}

/* Public: get the local-store buffer for a SPU thread (for use by
 * PPU-fallback handlers). Allocates on demand. */
uint8_t* spu_thread_get_local_store(uint32_t tid)
{
    return spu_thread_get_or_alloc_ls(spu_find_thread(tid));
}

uint32_t spu_thread_local_store_size(void) { return SPU_LS_SIZE; }

static uint16_t vm_read_be16(uint32_t a)
{
    extern uint8_t* vm_base;
    if (!vm_base || !a) return 0;
    const uint8_t* p = vm_base + a;
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* sys_spu_image_import(sys_spu_image_t* img, const void* src, uint32_t type)
 * (Lv2 System Call & Library Reference, p.108). Parse the SPU ELF at `src`
 * (guest memory) and fill the image-management struct so the entry point and
 * segment table are real -- previously this zeroed the struct, so every SPU
 * thread came up with entry=0, matched no fallback, and "instantly completed"
 * (cellmark's SPU benchmarks read 0 as a result).
 *
 * sys_spu_image  { u32 type; u32 entry_point; sys_spu_segment* segs; int nsegs; }
 * sys_spu_segment{ int type; u32 ls_start; int size; u64 src_pa; }  (0x18, src@0x10)
 * PT_LOAD -> COPY segment (src_pa = src + p_offset); a memsz>filesz tail -> a
 * FILL(0) segment, exactly as the SDK counts them. */
static int64_t sys_spu_image_import_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    uint32_t itype  = (uint32_t)ctx->gpr[5];   /* PROTECT(0) / DIRECT(1) */
    (void)itype;

    if (!img_ea || !src_ea || !vm_base) {
        if (img_ea && vm_base) memset(vm_base + img_ea, 0, 16);
        ctx->gpr[3] = (uint64_t)(int64_t)-14;  /* EFAULT */
        return -14;
    }

    /* Validate SPU ELF32 (big-endian) magic. */
    const uint8_t* e = vm_base + src_ea;
    if (!(e[0] == 0x7F && e[1] == 'E' && e[2] == 'L' && e[3] == 'F')) {
        memset(vm_base + img_ea, 0, 16);
        fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X -- not an ELF\n", img_ea, src_ea);
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)-8;   /* ENOEXEC */
        return -8;
    }

    uint32_t entry   = vm_read_be32(src_ea + 0x18);
    uint32_t phoff   = vm_read_be32(src_ea + 0x1C);
    uint16_t phentsz = vm_read_be16(src_ea + 0x2A);
    uint16_t phnum   = vm_read_be16(src_ea + 0x2C);
    if (phentsz == 0) phentsz = 0x20;

    /* Build the segment array in a dedicated guest scratch region (below the
     * TLS block at 0x0E000000). SPU images allow at most 32 segments. */
    static uint32_t s_spu_seg_bump = 0x0D000000u;
    uint32_t segs_ea = s_spu_seg_bump;
    int nsegs = 0;

    for (uint16_t i = 0; i < phnum && nsegs < 32; i++) {
        uint32_t ph = phoff + (uint32_t)i * phentsz;
        if (vm_read_be32(src_ea + ph + 0x00) != 1) continue;   /* PT_LOAD */
        uint32_t p_off = vm_read_be32(src_ea + ph + 0x04);
        uint32_t p_va  = vm_read_be32(src_ea + ph + 0x08);
        uint32_t p_fsz = vm_read_be32(src_ea + ph + 0x10);
        uint32_t p_msz = vm_read_be32(src_ea + ph + 0x14);

        uint32_t seg = segs_ea + (uint32_t)nsegs * 0x18;        /* COPY */
        vm_write_be32(seg + 0x00, 1);                           /* SYS_SPU_SEGMENT_TYPE_COPY */
        vm_write_be32(seg + 0x04, p_va);                        /* ls_start   */
        vm_write_be32(seg + 0x08, p_fsz);                       /* size       */
        vm_write_be32(seg + 0x10, 0);                           /* src_pa hi  */
        vm_write_be32(seg + 0x14, src_ea + p_off);              /* src_pa lo  */
        nsegs++;

        if (p_msz > p_fsz && nsegs < 32) {                      /* BSS tail -> FILL 0 */
            seg = segs_ea + (uint32_t)nsegs * 0x18;
            vm_write_be32(seg + 0x00, 2);                       /* SYS_SPU_SEGMENT_TYPE_FILL */
            vm_write_be32(seg + 0x04, p_va + p_fsz);            /* ls_start */
            vm_write_be32(seg + 0x08, p_msz - p_fsz);           /* size     */
            vm_write_be32(seg + 0x10, 0);                       /* value    */
            vm_write_be32(seg + 0x14, 0);
            nsegs++;
        }
    }
    s_spu_seg_bump += (uint32_t)nsegs * 0x18;
    if (s_spu_seg_bump >= 0x0E000000u) s_spu_seg_bump = 0x0D000000u;  /* wrap */

    vm_write_be32(img_ea + 0x00, 0);        /* type = SYS_SPU_IMAGE_TYPE_USER */
    vm_write_be32(img_ea + 0x04, entry);    /* entry_point */
    vm_write_be32(img_ea + 0x08, nsegs ? segs_ea : 0);  /* segs (guest EA) */
    vm_write_be32(img_ea + 0x0C, (uint32_t)nsegs);

    fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X -> entry=0x%05X nsegs=%d\n",
            img_ea, src_ea, entry, nsegs);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_image_open(*img, *path) — load an SPU ELF from the VFS, parse its
 * ELF32 header, and write the entry point + USER image type into the
 * sys_spu_image struct. The actual segment/code data isn't materialised
 * (we don't execute SPU); we only need entry to be correct so the SPU
 * PPU-fallback registry (ps3emu/spu_fallback.h) can match jobs by entry.
 *
 * sys_spu_image layout (16 bytes):
 *   +0  type    : u32  (0 = KERNEL, 1 = USER)
 *   +4  entry   : u32
 *   +8  segs    : u32 (EA of segment array, 0 if not materialised)
 *   +12 nsegs   : u32
 */
static int64_t sys_spu_image_open_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t img_ea  = (uint32_t)ctx->gpr[3];
    uint32_t path_ea = (uint32_t)ctx->gpr[4];

    if (img_ea && vm_base) {
        memset(vm_base + img_ea, 0, 16);
        vm_write_be32(img_ea + 0, 1);        /* type = USER */
    }

    if (!path_ea || !vm_base) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path=NULL — empty image\n", img_ea);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }

    const char* ps3_path = (const char*)(vm_base + path_ea);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    FILE* f = fopen(host_path, "rb");
    if (!f) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' (host: %s) — open failed\n",
                img_ea, ps3_path, host_path);
        fflush(stderr);
        /* Sony returns CELL_ENOENT for missing SPU images. Games often
         * pre-check, so a soft success keeps them moving. */
        ctx->gpr[3] = 0;
        return 0;
    }

    /* ELF32 header is 52 bytes. We need:
     *   +16  e_type    (2 bytes)   2 = ET_EXEC
     *   +18  e_machine (2 bytes)   23 = EM_SPU
     *   +24  e_entry   (4 bytes)
     */
    uint8_t hdr[52];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);

    uint32_t entry = 0;
    int valid_elf = 0;
    if (got >= 52 && hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        valid_elf = 1;
        /* Big-endian on PS3 */
        entry = ((uint32_t)hdr[24] << 24) |
                ((uint32_t)hdr[25] << 16) |
                ((uint32_t)hdr[26] <<  8) |
                ((uint32_t)hdr[27]);
    }

    if (img_ea && vm_base) {
        vm_write_be32(img_ea + 4, entry);
    }

    fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' entry=0x%08X%s\n",
            img_ea, ps3_path, entry, valid_elf ? "" : " (header invalid — entry left 0)");
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* Catch-all stub for SPU syscalls we don't model individually yet. */
static int64_t sys_spu_thread_stub(ppu_context* ctx)
{
    (void)ctx;
    ctx->gpr[3] = 0;
    return 0;
}

void lv2_register_all_syscalls(lv2_syscall_table* tbl)
{
    /* Initialize the table with unimplemented stubs first */
    lv2_syscall_table_init(tbl);

    /* Thread management */
    sys_ppu_thread_init(tbl);

    /* Synchronization primitives */
    sys_mutex_init(tbl);
    sys_cond_init(tbl);
    sys_semaphore_init(tbl);
    sys_rwlock_init(tbl);

    /* Timer and time (registered before events so event handlers
     * override the conflicting syscall numbers 141, 142, 145) */
    sys_timer_init(tbl);

    /* Event queues, ports, and flags */
    sys_event_init(tbl);

    /* Memory management */
    sys_memory_init(tbl);
    sys_vm_init(tbl);

    /* Filesystem */
    sys_fs_init(tbl);

    /* TTY (debug console I/O — used by CRT startup) */
    lv2_syscall_register(tbl, SYS_TTY_READ,  sys_tty_read);
    lv2_syscall_register(tbl, SYS_TTY_WRITE, sys_tty_write);
    /* Some SDK-era CRTs (Tokyo Jungle, Sonic/Gunstar hubs, 4 Elements HD) issue
     * sys_tty_write under the alternate number 988 (0x3DC) instead of 403; an
     * unimplemented return derails the CRT init table-walk into abort(). Alias it. */
    lv2_syscall_register(tbl, 988, sys_tty_write);

    /* SPU syscalls — we don't execute SPU code but the PPU-side wrappers
     * need consistent IDs and out-params. See the stateful group tracker
     * above for contract notes. */
    lv2_syscall_register(tbl, 169,                            sys_spu_thread_stub); /* deprecated */
    lv2_syscall_register(tbl, SYS_SPU_INITIALIZE,             sys_spu_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_OPEN,             sys_spu_image_open_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_IMPORT,           sys_spu_image_import_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_CLOSE,            sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CREATE,    sys_spu_thread_group_create_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DESTROY,   sys_spu_thread_group_destroy_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_START,     sys_spu_thread_group_start_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_SUSPEND,   sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_RESUME,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_YIELD,     sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_TERMINATE, sys_spu_thread_group_terminate_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_JOIN,      sys_spu_thread_group_join_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_INITIALIZE,      sys_spu_thread_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_SET_ARGUMENT,    sys_spu_thread_set_argument_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GET_EXIT_STATUS, sys_spu_thread_get_exit_status_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_CONNECT_EVENT,   sys_spu_thread_connect_event_handler);
    { extern void (*g_spu_out_mbox_hook)(uint32_t,uint32_t,int,uint32_t);
      g_spu_out_mbox_hook = ydkj_spu_out_mbox_deliver; }
    lv2_syscall_register(tbl, SYS_SPU_THREAD_DISCONNECT_EVENT,sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT, sys_spu_thread_group_connect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT, sys_spu_thread_group_disconnect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_LS,        sys_spu_thread_write_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_READ_LS,         sys_spu_thread_read_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SNR,       sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_BIND_QUEUE,      sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_UNBIND_QUEUE,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS, sys_spu_thread_group_connect_event_handler);
}

/* ---------------------------------------------------------------------------
 * Boot-harness wiring
 *
 * The recompiled games call lv2_syscall() (defined in the PPU boot harness,
 * runtime/ppu/ppu_loader.cpp). That harness now consults this global table via
 * lv2_try_syscall(), so the CRT's semaphore / mutex / memory / fs syscalls hit
 * the real implementations registered above instead of a return-0 logger stub.
 * Call lv2_init_syscalls() once at startup.
 * -----------------------------------------------------------------------*/
lv2_syscall_table g_lv2_syscalls;

void lv2_init_syscalls(void)
{
    lv2_register_all_syscalls(&g_lv2_syscalls);
}

/* Returns 1 (and sets gpr[3] from the handler) if `num` is a registered
 * syscall; 0 if unregistered (the caller keeps its own stub behaviour). The
 * comparison against the static-inline sentinel is reliable here because this
 * TU and lv2_syscall_table_init share the same instance. */
int lv2_try_syscall(ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];
    if (num >= LV2_SYSCALL_MAX)
        return 0;
    lv2_syscall_fn h = g_lv2_syscalls.handlers[num];
    if (!h || h == lv2_syscall_unimplemented)
        return 0;
    ctx->gpr[3] = (uint64_t)h(ctx);
    return 1;
}
