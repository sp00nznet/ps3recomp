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

#include "../memory/guest_mem_atomic.h"
#include <stdlib.h>   /* calloc, free */
#include "ps3emu/nid.h"   /* ps3_compute_nid (static inline) */
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
extern void sys_rsx_init(lv2_syscall_table* tbl);   /* libs/video/sys_rsx.c */
extern void sys_raw_spu_init(lv2_syscall_table* tbl);   /* runtime/spu/spu_raw.c */
extern void spu_raw_note_image(uint32_t src_ea, uint32_t entry);
#include "lv2_spu_image.h"

/* Guest scratch for an SPU ELF staged by sys_spu_image_open, which reads the
 * file in and then re-enters the import path with it. Shares the 16 MB region
 * below the TLS block (0x0E000000) with the segment tables built by
 * sys_spu_image_import: those grow up from 0x0D000000, these from the halfway
 * mark, so the two only meet after ~350k segment records. */
#define SPU_IMAGE_STAGE_BASE 0x0D800000u
#define SPU_IMAGE_STAGE_END  0x0E000000u
#define SPU_IMAGE_STAGE_MAX  0x00100000u   /* 1 MB: an SPU image is <=256 KB + headers */
#include "ps3emu/spu_fallback.h"
#include "../spu/spu_lifted_job.h"   /* spu_run_interp_job — run un-lifted SPU images */
#include "../spu/spu_lifted_thread.h" /* run a thread's own image, lifted */
#include "../spu/spu_context.h"       /* the architectural context that runs it */
#include "../spu/spu_workload.h"   /* the content-fingerprint registry */
#include "sys_event.h"

#include <stdio.h>
#include <string.h>

#include "../platform/win32_compat.h"   /* QueryPerformanceCounter shim for the SPU_SPEED timing */

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
        /* TTY_BT=<substring>: dump the guest LR back-chain whenever the title
         * prints a line containing it. The two hooks below do exactly this for
         * one hardcoded string each, which only ever helped the title they were
         * written for. A title's own error message is the cheapest breakpoint
         * there is -- it fires exactly when the thing went wrong, in the thread
         * it went wrong on -- so make it a knob rather than an edit.
         *
         * Virtua Fighter 5: TTY_BT="Command Buffer Overflow" names the AMGL
         * function whose free-space check is failing. */
        { static const char* pat = (const char*)1;
          if (pat == (const char*)1) pat = getenv("TTY_BT");
          if (pat && *pat && len < 4096) {
              char tmp[512]; uint32_t n = len < 511 ? len : 511;
              memcpy(tmp, vm_base + buf_ea, n); tmp[n] = 0;
              if (strstr(tmp, pat)) {
                  static int _n = 0;
                  if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 4) {
                      uint32_t sp = (uint32_t)ctx->gpr[1];
                      fprintf(stderr, "[TTY_BT] \"%.70s\" tid=%llu cia=0x%08X lr=0x%08X chain:",
                              tmp, (unsigned long long)ctx->thread_id,
                              (uint32_t)ctx->cia, (uint32_t)ctx->lr);
                      for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                          uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                          nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                          if (nsp <= sp || nsp >= 0x10000000u) break;
                          uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                          lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                          fprintf(stderr, " %08X", lr); sp = nsp;
                      }
                      fprintf(stderr, "\n"); fflush(stderr);
                  }
              }
          } }

        /* CRI error back-chain (YDKJ_CRIBT=1): dump the guest LR chain when a CRI
         * null-pointer / criFs error is printed, to locate the failing call. */
        if (getenv("YDKJ_CRIBT") && len < 4096) {
            char tmp[256]; uint32_t n = len < 255 ? len : 255;
            memcpy(tmp, vm_base + buf_ea, n); tmp[n] = 0;
            if (strstr(tmp, "NULL pointer") || strstr(tmp, "E2004090") || strstr(tmp, "CRICRS")) {
                static int _cb = 0; if (__atomic_fetch_add(&_cb, 1, __ATOMIC_RELAXED) < 4) {
                    uint32_t sp = (uint32_t)ctx->gpr[1];
                    fprintf(stderr, "[CRIBT] \"%.60s\" cia=0x%08X lr=0x%08X chain:", tmp,
                            (uint32_t)ctx->cia, (uint32_t)ctx->lr);
                    for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                        uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                        nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                        if (nsp <= sp || nsp >= 0x10000000u) break;
                        uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                        lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                        fprintf(stderr, " %08X", lr); sp = nsp;
                    }
                    fprintf(stderr, "\n"); fflush(stderr);
                }
            }
        }
        /* YDKJ_ASSERTBT: the libspurs _cellSpursIsLaunchedFromTuner assertion (which
         * aborts the SPURS task subsystem) prints through here. Dump the guest LR +
         * back-chain to locate the asserting function so it can be suppressed. */
        if (getenv("YDKJ_ASSERTBT") && len < 4096) {
            char tmp[256]; uint32_t n = len < 255 ? len : 255;
            memcpy(tmp, vm_base + buf_ea, n); tmp[n] = 0;
            if (strstr(tmp, "ASSERT") || strstr(tmp, "Tuner") || strstr(tmp, "usertrace") ||
                strstr(tmp, "libspurs")) {
                static int _ab = 0; if (__atomic_fetch_add(&_ab, 1, __ATOMIC_RELAXED) < 4) {
                    uint32_t sp = (uint32_t)ctx->gpr[1];
                    fprintf(stderr, "\n[ASSERTBT] \"%.70s\" cia=0x%08X lr=0x%08X chain:", tmp,
                            (uint32_t)ctx->cia, (uint32_t)ctx->lr);
                    for (int i = 0; i < 28 && sp && sp < 0x10000000u; i++) {
                        uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                        nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                        if (nsp <= sp || nsp >= 0x10000000u) break;
                        uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                        lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                        fprintf(stderr, " %08X", lr); sp = nsp;
                    }
                    fprintf(stderr, "\n"); fflush(stderr);
                }
            }
        }
        /* POOL CORRUPTION TRACE: when the game's debug allocator reports a bad
         * block / wrong pool / zeroed sentinel, dump the host call chain (resolved
         * to guest funcs) to find who passed/corrupted the block. */
        if (len < 4096) {
            char ptmp[256]; uint32_t pn = len < 255 ? len : 255;
            memcpy(ptmp, vm_base + buf_ea, pn); ptmp[pn] = 0;
            if (strstr(ptmp, "Pool possibly") || strstr(ptmp, "Bad signature") ||
                strstr(ptmp, "double-deallocate") ||
                strstr(ptmp, "out of memory on request")) {
                extern void ppu_log_host_chain(const char*);
                static int _pn = 0;
                if (__atomic_fetch_add(&_pn, 1, __ATOMIC_RELAXED) < 3) { fprintf(stderr, "[POOLTRACE] %.90s\n", ptmp); ppu_log_host_chain("pool-corrupt"); }
            }
        }
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
                strstr(tmp, "PSpu") || strstr(tmp, "ation") || strstr(tmp, "Mystery")) {
                uint32_t sp = (uint32_t)ctx->gpr[1];
                fprintf(stderr, "[pssg-bt] tty_write \"%.50s\" lr=0x%08X\n", tmp, (uint32_t)ctx->lr);
                /* The back-chain below is unreliable under the DRAIN/fragment
                 * model (LR slots read 0); the host-backtrace mapper is not. */
                { extern void ppu_guest_callstack(const char*); ppu_guest_callstack("tty"); }
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

/* The lv2 run states (RPCS3's lv2 is the reference for the kernel): a group is
 * NOT_INITIALIZED until every one of its threads has been initialized, then
 * INITIALIZED -- startable, joinable, destroyable -- and returns there once a
 * run is over -- when its last thread stops, joined or not -- so it can be
 * started again; the run's result waits for one join (join_state). */
#define SPU_GROUP_STATE_NOT_INITIALIZED 0
#define SPU_GROUP_STATE_INITIALIZED  1
#define SPU_GROUP_STATE_READY        2
#define SPU_GROUP_STATE_RUNNING      3
#define SPU_GROUP_STATE_STOPPED      4
#define SPU_GROUP_STATE_DESTROYED    5

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
#  include <errno.h>
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
    /* The four u64 arguments, COPIED when the thread is initialized rather than
     * read at start time. lv2 copies them there, and games reuse one guest args
     * block for every thread in a group, rewriting it between calls -- reading
     * it lazily hands every thread the last thread's values. */
    uint64_t args[4];
    uint32_t run_gen;        /* the group run this thread's host thread belongs to */
    int      host_live;      /* a host thread exists and has not been reaped */
    uint32_t img_ea;         /* sys_spu_image descriptor EA it was initialized with */
    /* The image, COPIED at initialize as lv2 does: the descriptor (and a USER
     * image's segment list) may be freed or reused once the thread exists. */
    uint32_t    nsegs;
    lv2_spu_seg segs[LV2_SPU_MAX_SEGS];
    uint32_t    img_src;     /* EA of the ELF the image came from, 0 unknown */
    /* Real SPU execution: the architectural context a thread running its own
     * lifted image owns, allocated at group_start and holding that thread's
     * local store. NULL for fallback and interpreter threads. */
    struct spu_context* sctx;
    uint64_t spu_cfg;        /* sys_spu_thread_{set,get}_spu_cfg */
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
    /* Live spu_context while a lifted worker is running on its host thread
     * (a stack local there), so the PPU can poke its mailbox and wake it. */
    void*               live_ctx;
    /* Command written by sys_spu_thread_write_spu_mb and not yet consumed.
     * Handed to the next lifted run as its inbound mailbox word. */
    uint32_t            pending_inmbox;
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
    /* sys_spu_thread_connect_event(id, eq, et, spup) binds ONE queue per
     * SPU PORT, and a thread commonly has several. MultiStream binds spup
     * 0x2A to its command/completion queue and spup 0x01 to its printf
     * queue. Keeping a single connected_queue let the second bind clobber
     * the first, so every reply went to the printf server and the PPU
     * waiting on the command queue never woke. */
    struct { uint32_t spup; uint32_t queue; } evt_bind[8];
    int                 evt_bind_n;
    /* sys_spu_thread_bind_queue(id, spuq, spuq_num): the event queues this
     * thread receives from with sys_spu_thread_receive_event(spuq_num). */
    struct { uint32_t num; uint32_t queue; } q_bind[16];
    int                 q_bind_n;
} spu_thread_t;

typedef struct {
    int      in_use;
    uint32_t id;
    int      state;
    uint32_t num_threads;
    uint32_t thread_indices[8];  /* table index into s_spu_threads */
    uint32_t init_mask;          /* thread slots initialized so far */
    char     name[32];
    int32_t  exit_status;        /* final ppu-side status the group reports */
    uint32_t cause;              /* how the group ended */
    /* Event queue connected via sys_spu_thread_group_connect_event[_all_threads].
     * When the group transitions to STOPPED (in group_join), an event is pushed
     * into this queue with source = SYS_SPU_THREAD_GROUP_EVENT (0x100..) so
     * PPU code blocked on sys_event_queue_receive wakes up. */
    uint32_t event_queue_id;
    /* Queue connected for SYS_SPU_THREAD_GROUP_EVENT_RUN (et 1): lv2 sends
     * {0xFFFFFFFF53505500, group id, 0, 0} to it when the group starts (as
     * RPCS3; checked by tests/conformance/mc). Nothing is sent at join. */
    uint32_t run_queue_id;
    uint32_t user_event_ports[64];
    int32_t  type;               /* SYS_SPU_THREAD_GROUP_TYPE_* */
    /* The run in progress and its end, as lv2 keeps them: a run ends when its
     * last thread stops (or on group exit / terminate), whether or not anyone
     * is joining; the group is then INITIALIZED again and the result waits in
     * join_state for one join to take it. */
    uint32_t run_gen;            /* bumped when a run ends */
    uint32_t running;            /* threads of the current run still going */
    uint32_t join_state;         /* cause of a finished run nobody joined yet */
    int      has_waiter;         /* a thread is blocked in group_join */
    int      waiter_done;
    uint32_t waiter_cause;
    int32_t  waiter_status;
    int      has_sched;          /* context-switched (not NON_CONTEXT) */
} spu_group_t;

static spu_group_t  s_spu_groups[MAX_SPU_GROUPS];
static spu_thread_t s_spu_threads[MAX_SPU_THREADS];
static uint32_t     s_spu_next_group_id  = 0x1000;
static uint32_t     s_spu_next_thread_id = 0x2000;
static int          s_spu_initialized    = 0;

/* Table slots are claimed and published without a lock: several PPU threads
 * create, look up and destroy groups and threads at once (lv2 serialises
 * these per object, not globally). in_use is the publication flag -- 0 free,
 * 2 being filled in, 1 live -- so a lookup that sees 1 (acquire) sees the
 * slot's id and fields as written before the release that set it. */
#define SLOT_LIVE(p)      (__atomic_load_n(&(p)->in_use, __ATOMIC_ACQUIRE) == 1)
#define SLOT_PUBLISH(p)   __atomic_store_n(&(p)->in_use, 1, __ATOMIC_RELEASE)
#define SLOT_FREE(p)      __atomic_store_n(&(p)->in_use, 0, __ATOMIC_RELEASE)
static int slot_claim(int* in_use)
{
    int z = 0;
    return __atomic_compare_exchange_n(in_use, &z, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
/* Zero a claimed slot without touching in_use (the first member). */
#define SLOT_CLEAR(p) memset((char*)(p) + sizeof((p)->in_use), 0, sizeof(*(p)) - sizeof((p)->in_use))

static spu_group_t* spu_find_group(uint32_t id)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (SLOT_LIVE(&s_spu_groups[i]) && s_spu_groups[i].id == id)
            return &s_spu_groups[i];
    }
    return NULL;
}

static spu_group_t* spu_alloc_group(void)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        spu_group_t* g = &s_spu_groups[i];
        if (slot_claim(&g->in_use)) {
            SLOT_CLEAR(g);
            g->id          = __atomic_fetch_add(&s_spu_next_group_id, 1, __ATOMIC_RELAXED);
            g->state       = SPU_GROUP_STATE_NOT_INITIALIZED;
            g->exit_status = 0;
            g->cause       = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
            SLOT_PUBLISH(g);
            return g;
        }
    }
    return NULL;
}

static spu_thread_t* spu_find_thread(uint32_t tid)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (SLOT_LIVE(&s_spu_threads[i]) && s_spu_threads[i].tid == tid)
            return &s_spu_threads[i];
    }
    return NULL;
}

static spu_thread_t* spu_alloc_thread(void)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        spu_thread_t* t = &s_spu_threads[i];
        if (slot_claim(&t->in_use)) {
            SLOT_CLEAR(t);
            t->tid = __atomic_fetch_add(&s_spu_next_thread_id, 1, __ATOMIC_RELAXED);
            SLOT_PUBLISH(t);
            return t;
        }
    }
    return NULL;
}

static void vm_write_be32(uint32_t guest_addr, uint32_t val)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return;
    gm_store32(vm_base + guest_addr, __builtin_bswap32(val));   /* single-copy atomic */
}

static uint32_t vm_read_be32(uint32_t guest_addr)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return 0;
    return __builtin_bswap32(gm_load32(vm_base + guest_addr));
}

/* syscall 872: sys_ss_get_open_psid(CellSsOpenPSID* psid { u64 high; u64 low })
 * Returns the console "Open PSID" (a per-console PSN/NP identity, also used for
 * save-data/trophy keying). The prior unimplemented stub returned without
 * touching the out-param, so the caller (an LBP 1.30 boot job) read heap
 * garbage. Fill it — zeros, matching RPCS3's default unconfigured console_psid —
 * and return CELL_OK. */
/* Online, every player needs their own: titles use it to tell consoles apart.
 * Simpsons Arcade hashes it into the key each player announces when joining a
 * match, and with every console at zero the host took the joining player for
 * itself and never gave them a slot. PS3_OPEN_PSID=<32 hex digits> sets it;
 * otherwise the player's name derives one (np_psnr_identity: --username,
 * PS3_NP_ONLINE_ID, or the OS login when online); otherwise it stays zero, so
 * offline saves keyed on it are untouched. */
extern const char* np_psnr_identity(void);   /* libs/network/np_psnr.c */
static void open_psid(uint32_t w[4])
{
    const char* e = getenv("PS3_OPEN_PSID");
    const char* id = np_psnr_identity();
    w[0] = w[1] = w[2] = w[3] = 0;
    if (e && strlen(e) >= 32) {
        for (int i = 0; i < 4; i++) {
            char part[9];
            memcpy(part, e + i * 8, 8);
            part[8] = 0;
            w[i] = (uint32_t)strtoul(part, NULL, 16);
        }
    } else if (id && *id) {
        uint32_t h = 2166136261u;   /* FNV-1a, re-seeded per word */
        for (int i = 0; i < 4; i++) {
            for (const char* p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
            w[i] = h;
            h ^= 0x9E3779B9u;
        }
    }
}

static int64_t sys_ss_get_open_psid_handler(ppu_context* ctx)
{
    uint32_t ptr = (uint32_t)ctx->gpr[3];
    if (ptr) {
        uint32_t w[4];
        open_psid(w);
        vm_write_be32(ptr + 0,  w[0]);   /* high[63:32] */
        vm_write_be32(ptr + 4,  w[1]);   /* high[31:0]  */
        vm_write_be32(ptr + 8,  w[2]);   /* low[63:32]  */
        vm_write_be32(ptr + 12, w[3]);   /* low[31:0]   */
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
    return 0;
}

/* Evaluates v once: it is often a call whose arguments are read from r3. */
#define LV2_RET(ctx, v) do { const int32_t lv2_ret_ = (int32_t)(v); \
    (ctx)->gpr[3] = (uint64_t)(int64_t)lv2_ret_; return lv2_ret_; } while (0)

/* SPU limits (RPCS3's spu_limits_t): of the six SPUs a process may use,
 * max_raw are raw SPUs and max_spu the rest. Context-switched groups share
 * SPUs, so they count as the largest of them; non-context groups occupy
 * theirs; a cooperate-with-system group is one shared SPU plus num-1
 * occupied ones, and there may be only one. */
static uint32_t s_spu_max_spu = 6, s_spu_max_raw = 0;

static int spu_limits_busy(uint32_t spu_limit, uint32_t raw_limit,
                           uint32_t physical, uint32_t controllable, uint32_t system_coop)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        const spu_group_t* g = &s_spu_groups[i];
        if (!SLOT_LIVE(g)) continue;
        if (g->type & 0x20) {
            system_coop++;
            if (controllable < 1) controllable = 1;
            physical += g->num_threads - 1;
        } else if (g->has_sched) {
            if (controllable < g->num_threads) controllable = g->num_threads;
        } else {
            physical += g->num_threads;
        }
    }
    return spu_limit + raw_limit > 6 || physical >= spu_limit || controllable > spu_limit ||
           system_coop > 1;
}

/* sys_spu_initialize(max_usable_spu, max_raw_spu) */
static int64_t sys_spu_initialize_handler(ppu_context* ctx)
{
    const uint32_t max_raw = (uint32_t)ctx->gpr[4];
    if (max_raw > 5) LV2_RET(ctx, CELL_EINVAL);
    if (spu_limits_busy(6 - max_raw, max_raw, 0, 0, 0)) LV2_RET(ctx, CELL_EBUSY);
    s_spu_max_raw = max_raw;
    s_spu_max_spu = 6 - max_raw;
    s_spu_initialized = 1;
    LV2_RET(ctx, CELL_OK);
}

/* sys_spu_thread_group_create(out_id_ea, num, prio, attr_ea)
 *
 * lv2 signature per RPCS3's sys_spu.h (oracle, no code copied): r5 is the
 * group PRIORITY (an int), NOT a name pointer. The name lives inside the
 * attribute struct, sys_spu_thread_group_attribute (BE):
 *   +0 u32 nsize (name length incl. NUL), +4 u32 name ptr, +8 s32 type.
 * The previous version read r5 directly as name_ea, so it could never
 * read a real group name (it dereferenced the priority integer as a
 * pointer) and never captured the priority at all. */
static int64_t sys_spu_thread_group_create_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t out_ea   = (uint32_t)ctx->gpr[3];
    uint32_t num      = (uint32_t)ctx->gpr[4];
    int32_t  prio     = (int32_t)ctx->gpr[5];
    uint32_t attr_ea  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[SPU] thread_group_create(num=%u prio=%d)\n", num, prio);

    /* lv2's checks (RPCS3 sys_spu_thread_group_create), for a process
     * without root permission. Memory containers are not modelled: a
     * MEMORY_FROM_CONTAINER group is accepted without charging one. */
    const uint32_t nsize   = vm_read_be32(attr_ea + 0);
    uint32_t       name_ea = vm_read_be32(attr_ea + 4);
    const int32_t  gtype   = (int32_t)vm_read_be32(attr_ea + 8);
    if (nsize > 0x80 || !num) LV2_RET(ctx, CELL_EINVAL);
    uint32_t max_threads = 6, min_threads = 1;
    int needs_root = 0, sched = 1;
    switch (gtype) {
    case 0x0: case 0x4: case 0x18:
        break;
    case 0x20: case 0x22: case 0x24: case 0x26:
        needs_root = gtype == 0x22 || gtype == 0x26;
        min_threads = 2;
        break;
    case 0x2: case 0x6: case 0xA: case 0x102: case 0x106: case 0x10A:
    case 0x202: case 0x206: case 0x20A: case 0x902: case 0x906:
    case 0xA02: case 0xA06: case 0xC02: case 0xC06:
        if (gtype & 0x700) max_threads = 1;
        needs_root = 1;
        break;
    default:
        LV2_RET(ctx, CELL_EINVAL);
    }
    const int coop = (gtype & 0x20) != 0;
    if (!coop && (gtype & 0x8)) sched = 0;
    if (num < min_threads || num > max_threads || needs_root ||
        (sched && !coop && (prio > 255 || prio < 16)))
        LV2_RET(ctx, CELL_EINVAL);
    {
        const uint32_t physical = coop ? num - 1 : sched ? 0 : num;
        const uint32_t controllable = coop ? 1 : sched ? num : 0;
        if (s_spu_max_spu + s_spu_max_raw > 6 || physical > s_spu_max_spu || controllable > s_spu_max_spu)
            LV2_RET(ctx, CELL_EINVAL);
        if (spu_limits_busy(s_spu_max_spu, s_spu_max_raw, physical, controllable,
                            controllable && physical ? 1u : 0u))
            LV2_RET(ctx, CELL_EBUSY);
    }

    spu_group_t* g = spu_alloc_group();
    if (!g) LV2_RET(ctx, CELL_EAGAIN);
    g->num_threads = num;
    g->type        = gtype;
    g->has_sched   = sched;
    if (!nsize) name_ea = 0;
    if (name_ea && vm_base) {
        const char* src = (const char*)(vm_base + name_ea);
        size_t i = 0;
        for (; i < sizeof(g->name) - 1 && src[i]; i++)
            g->name[i] = src[i];
        g->name[i] = 0;
    }

    vm_write_be32(out_ea, g->id);

    fprintf(stderr, "[SPU] group_create -> id=0x%X num=%u prio=%d type=0x%X name=%.31s\n",
            g->id, num, prio, gtype, g->name);
    fflush(stderr);
    LV2_RET(ctx, CELL_OK);
}


/* sys_spu_thread_initialize(out_tid_ea, group_id, thread_num, img_ea, attr_ea, args_ea) */

/* sys_spu_thread_initialize(thread*, group, spu_num, img*, attr*, arg*): the
 * kernel's checks (RPCS3's lv2 is the reference for the kernel), then the
 * image and arguments are copied into the thread. */
static int64_t sys_spu_thread_initialize_handler(ppu_context* ctx)
{
    uint32_t out_tid_ea = (uint32_t)ctx->gpr[3];
    uint32_t group_id   = (uint32_t)ctx->gpr[4];
    uint32_t thread_num = (uint32_t)ctx->gpr[5];
    uint32_t img_ea     = (uint32_t)ctx->gpr[6];
    uint32_t attr_ea    = (uint32_t)ctx->gpr[7];
    uint32_t args_ea    = (uint32_t)ctx->gpr[8];

    if (thread_num >= 8) LV2_RET(ctx, CELL_EINVAL);
    if (!attr_ea || !args_ea || !img_ea || !out_tid_ea) LV2_RET(ctx, CELL_EFAULT);
    if (vm_read_be32(attr_ea + 4) > 0x80) LV2_RET(ctx, CELL_EINVAL);           /* name_len */
    if (vm_read_be32(attr_ea + 8) & ~0x3u) LV2_RET(ctx, CELL_EINVAL);          /* option */

    uint32_t entry = 0, nsegs = 0, src = 0;
    lv2_spu_seg segs[LV2_SPU_MAX_SEGS];
    const int32_t rr = lv2_spu_image_resolve(img_ea, &entry, segs, &nsegs, &src);
    if (rr) LV2_RET(ctx, rr);
    if (vm_read_be32(img_ea) == 0) {             /* USER segments are checked; KERNEL ones came from lv2 */
        int have_copy = 0, have_info = 0;
        for (uint32_t i = 0; i < nsegs; i++) {
            const lv2_spu_seg* g = &segs[i];
            if (g->type == LV2_SPU_SEG_COPY) {
                if (g->addr % 4) LV2_RET(ctx, CELL_EINVAL);
                have_copy = 1;
            } else if (g->type == LV2_SPU_SEG_INFO) {
                if (g->size > 256 || have_info) LV2_RET(ctx, CELL_EINVAL);
                have_info = 1;
                continue;
            } else if (g->type != LV2_SPU_SEG_FILL) {
                LV2_RET(ctx, CELL_EINVAL);
            }
            if (!g->size || (g->ls | g->size) % 0x10 || g->ls >= 0x40000 || g->size > 0x40000)
                LV2_RET(ctx, CELL_EINVAL);
            for (uint32_t j = 0; j < i; j++)
                if (segs[j].type != LV2_SPU_SEG_INFO &&
                    g->ls + g->size > segs[j].ls && segs[j].ls + segs[j].size > g->ls)
                    LV2_RET(ctx, CELL_EINVAL);       /* overlapping */
        }
        if (!have_copy) LV2_RET(ctx, CELL_EINVAL);
    }

    spu_group_t* g = spu_find_group(group_id);
    if (!g) LV2_RET(ctx, CELL_ESRCH);
    if (g->state != SPU_GROUP_STATE_NOT_INITIALIZED || (g->init_mask & (1u << thread_num)))
        LV2_RET(ctx, CELL_EBUSY);
    spu_thread_t* t = spu_alloc_thread();
    if (!t) LV2_RET(ctx, CELL_ENOMEM);
    t->group_id    = group_id;
    t->index       = thread_num;
    t->entry_point = entry;
    t->img_ea      = img_ea;
    t->nsegs       = nsegs;
    memcpy(t->segs, segs, sizeof(lv2_spu_seg) * nsegs);
    t->img_src     = src;
    t->args_ea     = args_ea;
    t->args_size   = 0;
    for (int a = 0; a < 4; a++) {
        uint64_t hi = vm_read_be32(args_ea + (uint32_t)a * 8);
        uint64_t lo = vm_read_be32(args_ea + (uint32_t)a * 8 + 4);
        t->args[a] = (hi << 32) | lo;
    }
    g->thread_indices[thread_num] = (uint32_t)(t - s_spu_threads);
    g->init_mask |= 1u << thread_num;
    if ((uint32_t)__builtin_popcount(g->init_mask) == g->num_threads)
        g->state = SPU_GROUP_STATE_INITIALIZED;
    vm_write_be32(out_tid_ea, t->tid);
    fprintf(stderr, "[SPU] thread_init group=0x%X index=%u img=0x%08X args=0x%08X -> tid=0x%X entry=0x%08X\n",
            group_id, thread_num, img_ea, args_ea, t->tid, t->entry_point);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* Reserved stack for an SPU host thread. Lifted SPU code turns a guest loop
 * into a chain of host calls, so a thread that gets the host default -- 512 KB
 * on Darwin against 16 MB on Windows -- dies inside code that has done nothing
 * wrong. SPU_HOST_STACK_BIG raises it further for chasing a runaway chain. */
static size_t spu_host_stack_bytes(void)
{
    return getenv("SPU_HOST_STACK_BIG") ? (size_t)512 * 1024 * 1024
                                   : (size_t)16 * 1024 * 1024;
}

#ifndef _WIN32
/* pthread_create with that stack, falling back to the host default if the size
 * is refused -- a thread with a small stack beats no thread at all. */
static void spu_spawn_host_thread(spu_thread_handle_t* out,
                                  void* (*fn)(void*), void* arg)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = EINVAL;
    if (pthread_attr_setstacksize(&attr, spu_host_stack_bytes()) == 0)
        rc = pthread_create(out, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        pthread_create(out, NULL, fn, arg);
}
#endif

static int32_t spu_interp_fallback(uint32_t tid, uint32_t args_ea, uint32_t args_size, void* user);

/* One lock and condition for every group's run state (start, thread
 * completion, join, terminate). */
static SRWLOCK            s_grp_lock = SRWLOCK_INIT;
/* Guards spu_thread_t.live_ctx. The context it points at is a stack local of
 * the run that published it: a PPU thread writing that context's mailbox or
 * signal registers holds this shared, and the run takes it exclusive to
 * unpublish, so it cannot return (and free the frame) mid-write. */
static SRWLOCK            s_live_ctx_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_grp_cv   = CONDITION_VARIABLE_INIT;

/* The run is over (lock held): settle cause and status, return the group to
 * INITIALIZED, and hand the result to a blocked joiner or keep it for the
 * next join. */
static void spu_group_run_end_locked(spu_group_t* g)
{
    if (g->cause != SPU_GROUP_CAUSE_GROUP_EXIT && g->cause != SPU_GROUP_CAUSE_TERMINATED) {
        g->cause       = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
        g->exit_status = 0;
    }
    g->running = 0;
    g->run_gen++;
    g->state = SPU_GROUP_STATE_INITIALIZED;
    if (g->has_waiter) {
        g->waiter_cause  = g->cause;
        g->waiter_status = g->exit_status;
        g->waiter_done   = 1;
        g->join_state    = 0;
    } else {
        g->join_state = g->cause;
    }
    WakeAllConditionVariable(&s_grp_cv);
}

/* Ask every running thread of the group to stop: each checks at its next
 * channel wait, interpreter step or lifted trampoline. Wakes the ones parked
 * in a channel read. Holds s_grp_lock (s_live_ctx_lock is taken inside).
 * Returns how many were asked. */
static int spu_group_request_stop_locked(spu_group_t* g)
{
    int asked = 0;
    AcquireSRWLockShared(&s_live_ctx_lock);
    for (int i = 0; i < 8; i++) {
        if (!(g->init_mask & (1u << i))) continue;
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!__atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) continue;
        spu_context* c = t->live_ctx ? (spu_context*)t->live_ctx : t->sctx;
        if (!c) continue;
        __atomic_store_n(&c->stop_request, 1u, __ATOMIC_RELEASE);
        extern void spu_ch_wake(spu_context*);
        spu_ch_wake(c);
        asked++;
    }
    ReleaseSRWLockShared(&s_live_ctx_lock);
    return asked;
}

/* Wait, bounded, until none of the group's threads is running. Holds
 * s_grp_lock on entry and exit, not while it sleeps. Returns 1 if they all
 * stopped. */
static int spu_group_wait_stopped_locked(spu_group_t* g, int max_ms)
{
    for (int ms = 0;; ms++) {
        int any = 0;
        for (int i = 0; i < 8; i++) {
            if (!(g->init_mask & (1u << i))) continue;
            uint32_t idx = g->thread_indices[i];
            if (idx < MAX_SPU_THREADS && __atomic_load_n(&s_spu_threads[idx].running, __ATOMIC_ACQUIRE))
                any = 1;
        }
        if (!any) return 1;
        if (ms >= max_ms) return 0;
        ReleaseSRWLockExclusive(&s_grp_lock);
#ifdef _WIN32
        Sleep(1);
#else
        { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
#endif
        AcquireSRWLockExclusive(&s_grp_lock);
    }
}

/* A thread of the run `gen` has stopped; a group exit it made ends the run. */
static void spu_group_thread_done(uint32_t group_id, uint32_t gen, int group_exit, int32_t gstatus)
{
    AcquireSRWLockExclusive(&s_grp_lock);
    spu_group_t* g = spu_find_group(group_id);
    if (g && g->state == SPU_GROUP_STATE_RUNNING && gen == g->run_gen) {
        if (group_exit) {
            g->cause       = SPU_GROUP_CAUSE_GROUP_EXIT;
            g->exit_status = gstatus;
            spu_group_request_stop_locked(g);       /* lv2 stops the other threads */
            spu_group_run_end_locked(g);
        } else if (--g->running == 0) {
            spu_group_run_end_locked(g);
        }
    }
    ReleaseSRWLockExclusive(&s_grp_lock);
}

/* Reap host threads of earlier runs that have finished. */
static void spu_group_reap(spu_group_t* g)
{
    for (int i = 0; i < 8; i++) {               /* slots are thread indices 0..7 */
        if (!(g->init_mask & (1u << i))) continue;
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!t->host_live || __atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) continue;
#ifdef _WIN32
        if (t->host_thread) { WaitForSingleObject(t->host_thread, INFINITE); CloseHandle(t->host_thread); }
        t->host_thread = NULL;
#else
        pthread_join(t->host_thread, NULL);
        pthread_mutex_destroy(&t->finish_event.mu);
        pthread_cond_destroy(&t->finish_event.cv);
#endif
        t->host_live = 0;
    }
}

/* Host-thread entry for a thread running its image's own lifted SPU code.
 *
 * Everything SPU here is in runtime/spu/spu_lifted_thread.c; this is the part
 * that belongs to the group state machine: apply the classified stop to the
 * thread and, for a GROUP_EXIT, to the group, then release group_join. */
#ifdef _WIN32
static DWORD WINAPI spu_exec_thread_proc(LPVOID arg)
#else
static void* spu_exec_thread_proc(void* arg)
#endif
{
#ifdef _WIN32
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }  /* let SO reach the reporter */
#endif
    spu_thread_t* t = (spu_thread_t*)arg;
    spu_lifted_thread_result r;
    spu_lifted_thread_run(t->sctx, &r);
    t->exit_status = r.exit_status;
#ifdef _WIN32
    __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
    SetEvent(t->finish_event);
#else
    pthread_mutex_lock(&t->finish_event.mu);
    __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
    t->finish_event.done = 1;
    pthread_cond_broadcast(&t->finish_event.cv);
    pthread_mutex_unlock(&t->finish_event.mu);
#endif
    spu_group_thread_done(t->group_id, t->run_gen, r.group_exit, r.group_status);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Host-thread entry for a PPU-fallback SPU thread. */
#ifdef _WIN32
static DWORD WINAPI spu_fallback_thread_proc(LPVOID arg)
#else
static void* spu_fallback_thread_proc(void* arg)
#endif
{
#ifdef _WIN32
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }  /* let SO reach the reporter */
#endif
    spu_thread_t* t = (spu_thread_t*)arg;
    int32_t rc = 0;
    if (t->fb_handler) {
        rc = t->fb_handler(t->tid, t->args_ea, t->args_size, t->fb_user);
    }
    t->exit_status = rc;
    /* EXPERIMENT (RD_SPU_DONE_EVENT): on SPU thread completion, deliver an event
     * to the connected queue -- PPU code may block in sys_event_queue_receive for
     * the SPU's completion signal (which normally comes from a WrOutIntrMbox the
     * SPU issues before finishing). Tests whether the render hang is that wait. */
    if (getenv("RD_SPU_DONE_EVENT") && t->connected_queue) {
        extern int sys_event_queue_push_by_id(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
        sys_event_queue_push_by_id(t->connected_queue,
            ((uint64_t)t->tid << 32) | 0x2u, (uint64_t)(uint32_t)rc, 0, 0);
        fprintf(stderr, "[SPU-DONE-EVT] tid=0x%X rc=0x%X -> queue=%u\n",
                t->tid, rc, t->connected_queue);
    }
    /* Mark complete, then let the group see the thread stop. */
    {
        extern SPU_THREAD_LOCAL int g_spu_interp_group_exit_valid;
        extern SPU_THREAD_LOCAL int32_t g_spu_interp_group_exit_status;
        const int gx = t->fb_handler == spu_interp_fallback && g_spu_interp_group_exit_valid;
        const int32_t gs = g_spu_interp_group_exit_status;
#ifdef _WIN32
        __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
        SetEvent(t->finish_event);
#else
        pthread_mutex_lock(&t->finish_event.mu);
        __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
        t->finish_event.done = 1;
        pthread_cond_broadcast(&t->finish_event.cv);
        pthread_mutex_unlock(&t->finish_event.mu);
#endif
        spu_group_thread_done(t->group_id, t->run_gen, gx, gs);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Load a sys_spu_image's segments into a 256 KB local store. COPY segments
 * (type 1) are memcpy'd from their guest source EA; FILL segments (type 2) are
 * zeroed. Mirrors sys_spu_image_import's segment layout {type,ls_start,size,
 * src(pa64)} (0x18 bytes each). Returns the entry point, or 0 on failure. */
static uint32_t spu_load_image_to_ls(const spu_thread_t* t, uint8_t* ls)
{
    if (!t || !ls) return 0;
    lv2_spu_load_segments(t->segs, t->nsegs, ls);
    return t->entry_point;
}

/* PPU-fallback that runs an un-lifted SPU thread via the interpreter. Registered
 * for the currently-instant-completing thread groups when RD_SPU_INTERP is set
 * (see group_start). Loads the thread's image into its LS and interprets from
 * the entry point; DMA/mailbox/event ops go through the shared channel ABI. */
static uint8_t* spu_thread_get_or_alloc_ls(spu_thread_t* t);   /* fwd (defined below) */
/* Lifted raw-SPU-thread runner (spu_lifted_fallback.c); declared here so
 * group_start can recognise it and run such workers synchronously. */
extern int32_t spu_registry_fallback(uint32_t, uint32_t, uint32_t, void*);

static int32_t spu_interp_fallback(uint32_t tid, uint32_t args_ea,
                                   uint32_t args_size, void* user)
{
    (void)args_size; (void)user;
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) return -1;
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) return -1;
    uint32_t entry = spu_load_image_to_ls(t, ls);
    if (getenv("SPU_ARGS_DUMP") && vm_base && args_ea) {
        fprintf(stderr, "[SPU-ARGS] tid=0x%X args@0x%08X:", tid, args_ea);
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", vm_read_be32(args_ea + i*4));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[SPU-INTERP] tid=0x%X entry=0x%05X img=0x%08X args=0x%08X -> interpreting\n",
            tid, entry, t->img_ea, args_ea);
    int32_t sc = spu_run_interp_job(ls, entry, t->args, -1,  /* pure interp: no fast-path rejoin */
                                    t->tid, t->group_id, 0); /* identify for mbox->event delivery */
    { extern SPU_THREAD_LOCAL uint32_t g_spu_interp_last_pc; extern SPU_THREAD_LOCAL uint64_t g_spu_interp_steps;
      fprintf(stderr, "[SPU-INTERP] tid=0x%X done (stop=0x%X, %llu insns, last pc=0x%05X)\n",
              tid, sc, (unsigned long long)g_spu_interp_steps, g_spu_interp_last_pc); }
    { extern SPU_THREAD_LOCAL int g_spu_interp_exit_valid;
      extern SPU_THREAD_LOCAL int32_t g_spu_interp_exit_status;
      if (g_spu_interp_exit_valid) return g_spu_interp_exit_status; }
    return sc;
}

/* Per-frame sim-SPU dispatch. The game runs its SPU jobs as persistent workers:
 * after init they stop, then each frame the game event-port-sends a work-
 * descriptor EA and waits on the SPU's completion queue. Re-run the SPU whose
 * connected completion queue is `comp_queue`, feeding `work_ea` into its inbound
 * mailbox (its first rdch InMbox), so it DMAs that frame's descriptor, computes,
 * and signals completion -- satisfying the PPU's wait. Returns 1 if dispatched. */
int spu_dispatch_frame_by_queue(uint32_t comp_queue, uint32_t work_ea)
{
    if (!getenv("RD_SPU_INTERP")) return 0;
    /* This re-runs a worker on the calling PPU thread, which is only right in
     * the synchronous sim mode, where the worker has no host thread of its
     * own. With real SPU threads (RD_SPU_INTERP_ASYNC) the worker is already
     * running and drains its own queue; a second run here would execute the
     * same SPU program twice at once. */
    if (getenv("RD_SPU_INTERP_ASYNC")) return 0;
    /* Two callers reach here: an event-port send, which carries the frame's
     * work descriptor in data2, and the blocking-receive path, which has none
     * and passes 0. A worker started without a descriptor reads an empty inbox,
     * takes the 0 as its descriptor EA and DMAs its results to the zero page --
     * hundreds of thousands of 128-byte PUTs to EA 0 and no output anywhere.
     * Remember the last descriptor seen per queue and reuse it when the
     * receive path re-runs the same worker. */
    static uint32_t last_work[64];
    if (comp_queue < 64) {
        if (work_ea) last_work[comp_queue] = work_ea;
        else         work_ea = last_work[comp_queue];
    }
    for (uint32_t i = 0; i < MAX_SPU_THREADS; i++) {
        spu_thread_t* t = &s_spu_threads[i];
        if (!SLOT_LIVE(t) || t->connected_queue != comp_queue || !t->img_ea) continue;
        uint8_t* ls = spu_thread_get_or_alloc_ls(t);
        if (!ls) return 0;
        uint32_t entry = spu_load_image_to_ls(t, ls);
        fprintf(stderr, "[SPU-FRAME] tid=0x%X q=%u work=0x%08X -> re-run\n",
                t->tid, comp_queue, work_ea);
        /* Seed the inbound mailbox with the work descriptor when we HAVE one.
         * spu_run_interp_job treats 0 as "do not seed", so the blocking-receive
         * dispatch -- which is called with no descriptor -- behaves exactly as
         * before, while an event-port send delivers its real data2.
         *
         * This used to be gated behind RD_SPU_FRAME_MBOX and passed 0 either
         * way, because force-seeding had made a worker spin. That spin was the
         * seed being 0: the receive path calls us with work_ea = 0, the solver
         * took that as its descriptor EA and DMA'd results to address 0 -- half
         * a million PUTs into the zero page, no progress, and the fluid's vertex
         * buffer left untouched. Seeding only a real descriptor keeps the
         * completion handshake intact and gives the workers their input. */
        /* SPU_WORKDESC_DUMP=1: the work descriptor the PPU hands the worker. Its
         * pointers are where the job DMAs from and to, so a job that writes
         * only scratch is either reading the wrong descriptor or the descriptor
         * does not name the buffer we expect. */
        { static _Atomic int _wd = -1; if (_wd < 0) _wd = getenv("SPU_WORKDESC_DUMP") ? 1 : 0;
          if (_wd && work_ea > 0x1000000u && vm_base) { static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 3) {
              fprintf(stderr, "[WORKDESC] tid=0x%X ea=0x%08X:%c", t->tid, work_ea, 10);
              for (int r = 0; r < 16; r++) {
                  fprintf(stderr, "  +0x%03X:", r * 16);
                  for (int c = 0; c < 4; c++)
                      fprintf(stderr, " %08X", vm_read_be32(work_ea + r*16 + c*4));
                  fprintf(stderr, "%c", 10);
              } } } }
        { static _Atomic int _wh = -1; if (_wh < 0) _wh = getenv("SPU_WORKDESC_HDR") ? 1 : 0;
          if (_wh && work_ea > 0x1000000u && vm_base) { static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 40)
              fprintf(stderr, "[WORKHDR] tid=0x%X ea=0x%08X w0=%u w1=%u f2=%g f3=%g%c",
                      t->tid, work_ea, vm_read_be32(work_ea), vm_read_be32(work_ea+4),
                      (double)*(const float*)&(const uint32_t){0}, 0.0, 10); }
          if (_wh && work_ea > 0x1000000u && vm_base) { } }
        { static _Atomic int _sd = -1; if (_sd < 0) _sd = getenv("SPU_SEED_DBG") ? 1 : 0;
          if (_sd) { static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 12)
              fprintf(stderr, "[SPU-SEED] tid=0x%X entry=0x%05X args=0x%08X seed=0x%08X%c",
                      t->tid, entry, t->args_ea, work_ea, 10); } }
        /* Do NOT seed the inbound mailbox. It is the reply channel for the
         * SPU's own sys_spu_thread_receive_event (stop 0x110) service, and the
         * worker's spu_printf helper treats a non-empty inbox as EBUSY. The
         * work descriptor reaches the SPU through that service instead. */
        LARGE_INTEGER _t0, _t1, _fq; QueryPerformanceCounter(&_t0);
        int32_t frc = spu_run_interp_job(ls, entry, t->args, -1, t->tid, t->group_id,
                                         getenv("RD_SPU_FRAME_MBOX") ? work_ea : 0u);
        { static _Atomic int _sp = -1; if (_sp < 0) _sp = getenv("SPU_SPEED") ? 1 : 0;
          if (_sp) { QueryPerformanceCounter(&_t1); QueryPerformanceFrequency(&_fq);
              extern SPU_THREAD_LOCAL uint64_t g_spu_interp_steps;
              double sec = (double)(_t1.QuadPart - _t0.QuadPart) / (double)_fq.QuadPart;
              static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 20)
                  fprintf(stderr, "[spu-speed] tid=0x%X %llu insns in %.3f s = %.1f M/s%c",
                          t->tid, (unsigned long long)g_spu_interp_steps, sec,
                          sec > 0 ? g_spu_interp_steps / sec / 1e6 : 0.0, 10); } }
        { extern SPU_THREAD_LOCAL uint32_t g_spu_interp_last_pc; extern SPU_THREAD_LOCAL uint64_t g_spu_interp_steps;
          fprintf(stderr, "[SPU-FRAME] tid=0x%X done (stop=0x%X, %llu insns, last pc=0x%05X)\n",
                  t->tid, frc, (unsigned long long)g_spu_interp_steps, g_spu_interp_last_pc); }
        return 1;
    }
    return 0;
}

/* sys_spu_thread_group_start(id) */
extern int sys_event_queue_push_by_id(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
static int64_t sys_spu_thread_group_start_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (!g) LV2_RET(ctx, CELL_ESRCH);
    spu_group_reap(g);
    AcquireSRWLockExclusive(&s_grp_lock);
    if (g->state != SPU_GROUP_STATE_INITIALIZED) {
        ReleaseSRWLockExclusive(&s_grp_lock);
        LV2_RET(ctx, CELL_ESTAT);
    }
    /* A new run: no cause yet, an unjoined result of the previous run is
     * dropped (lv2 clears join_state at start), and `running` holds a guard
     * reference until every thread is launched, so one that finishes at once
     * cannot end the run before its siblings have started. */
    g->state       = SPU_GROUP_STATE_RUNNING;
    g->cause       = 0;
    g->exit_status = 0;
    g->join_state  = 0;
    g->running     = 1;
    const uint32_t gen = g->run_gen;
    ReleaseSRWLockExclusive(&s_grp_lock);
    if (g->run_queue_id)
        sys_event_queue_push_by_id(g->run_queue_id, 0xFFFFFFFF53505500ull, (uint64_t)id, 0, 0);

    /* DIAG (YDKJ_INSTDUMP): dump the CellSpurs instance at group_start time, to
     * see whether libsre has populated it BEFORE the SPU kernel threads spawn.
     *
     * The address is one title's, and the read used to happen on every group
     * start of every title. A runtime whose guest memory is the full 4 GB
     * reservation gets away with that; one that maps only what it has handed
     * out faults here, in a diagnostic, before the title has done anything
     * wrong. So the read is off unless asked for, and it is bounds-checked
     * against what the runtime knows of the mapped range: ppu_vm_size is 0
     * where the whole 32-bit space is backed and there is nothing to check,
     * and otherwise the size of the arena. */
    { extern uint8_t* vm_base; extern uint32_t ppu_vm_size;
      static int s_d = 0;
      if (vm_base && s_d < 4) {
        s_d++;
        static _Atomic int _idump = -1;
        if (_idump < 0) _idump = getenv("YDKJ_INSTDUMP") ? 1 : 0;
        /* Dump BOTH candidate instance addrs: the real one is 0x40009F00 (init arg);
         * 0x40009D00 was the old hardcoded guess. See which libsre actually populated. */
        if (_idump) {
          for (uint32_t _ia = 0x40009D00; _ia <= 0x40009F00; _ia += 0x200) {
            if (ppu_vm_size && (uint64_t)_ia + 0xA0u > (uint64_t)ppu_vm_size) {
                fprintf(stderr, "[INSTDUMP] group_start id=0x%X CellSpurs@0x%08X is past "
                        "the mapped guest range (0x%08X), not read\n", id, _ia, ppu_vm_size);
                continue;
            }
            const uint8_t* in = vm_base + _ia;
            fprintf(stderr, "[INSTDUMP] group_start id=0x%X CellSpurs@0x%08X (0x140 bytes):\n", id, _ia);
            for (int row=0; row<10; row++){
                fprintf(stderr, "  +0x%03X:", row*16);
                for (int i=0;i<4;i++){ int o=row*16+i*4; uint32_t w=((uint32_t)in[o]<<24)|((uint32_t)in[o+1]<<16)|((uint32_t)in[o+2]<<8)|in[o+3]; fprintf(stderr," %08X",w);}
                fprintf(stderr, "\n");
            }
          }
          fflush(stderr);
        }
        /* Arm a page-guard on the instance page so we catch the libsre function
         * that writes the CellSpurs struct (WWATCH misses memcpy/DMA writes).
         * Its own lever, so it still arms with the dump off; it protects a page
         * rather than reading one, and is a no-op away from Windows. */
        if (getenv("YDKJ_GUARD_INST")) { extern void ppu_guard_page(uint32_t); ppu_guard_page(0x40009D00); }
        fflush(stderr);
      } }

    /* For each thread in the group, look up a registered PPU fallback by
     * the thread's SPU image entry point. Threads with a fallback run on
     * a host thread (real concurrency, like real SPUs). Threads without
     * a fallback complete instantly with status 0.
     * group_join() blocks until all spawned host threads finish. */
    int spawned = 0;
    int instant = 0;
    int nofb    = 0;   /* subset of `instant` that had no fallback at all */
    /* A thread sits at its own index (0..7), not packed below num_threads: lv2
     * lets a one-thread group use index 5, as inFamous's Bink group does. */
    for (uint32_t i = 0; i < 8; i++) {
        if (!(g->init_mask & (1u << i))) continue;
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!SLOT_LIVE(t)) continue;

        /* Real SPU execution first. If the title registered lifted code for
         * this thread's image, the thread runs THAT -- on its own host thread,
         * as a real SPU would, with group_join waiting on it exactly as it
         * waits on a fallback thread. Images with no lifted code fall through
         * to the fallback and interpreter paths below, unchanged. */
        /* Which lifted image is this? By content first: images very often
         * share an entry address (every plain SPU program starting at LS 0),
         * so "some image has a function at the entry" picks one of them
         * arbitrarily, and the thread would run another program's code. The
         * fingerprint names the image exactly; the entry lookup stays for
         * images registered without one. */
        int fp_img = 0;
        if (t->img_src && vm_base) {
            size_t isz = spu_elf_image_size(vm_base + t->img_src, 1u << 20);
            if (isz && !spu_workload_find_img(spu_workload_fingerprint(vm_base + t->img_src, isz), &fp_img))
                fp_img = 0;
        }
        if (fp_img > 0 || spu_lifted_thread_available(t->entry_point)) {
            if (!t->sctx) t->sctx = (spu_context*)calloc(1, sizeof(spu_context));
            if (t->sctx) {
                spu_lifted_thread_desc d;
                d.tid      = t->tid;
                d.group_id = id;
                d.entry    = t->entry_point;
                d.img_ea   = t->img_ea;
                d.image_id = fp_img;
                d.segs     = (const struct lv2_spu_seg_s*)t->segs;
                d.nsegs    = t->nsegs;
                for (int a = 0; a < 4; a++) d.args[a] = t->args[a];
                spu_lifted_thread_setup(t->sctx, &d);
                /* Logged before the thread starts: it owns the registers after. */
                fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X "
                        "args=0x%08X -> LIFTED SPU execution (image %d, "
                        "r3=0x%08X%08X r4=0x%08X%08X r5=0x%08X%08X r6=0x%08X%08X)\n",
                        id, t->tid, t->entry_point, t->args_ea, t->sctx->image_id,
                        t->sctx->gpr[3]._u32[0], t->sctx->gpr[3]._u32[1],
                        t->sctx->gpr[4]._u32[0], t->sctx->gpr[4]._u32[1],
                        t->sctx->gpr[5]._u32[0], t->sctx->gpr[5]._u32[1],
                        t->sctx->gpr[6]._u32[0], t->sctx->gpr[6]._u32[1]);
                __atomic_store_n(&t->running, 1, __ATOMIC_RELEASE);
                t->run_gen = gen;
                t->host_live = 1;
                AcquireSRWLockExclusive(&s_grp_lock); g->running++; ReleaseSRWLockExclusive(&s_grp_lock);
#ifdef _WIN32
                if (!t->finish_event)
                    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
                else
                    ResetEvent(t->finish_event);
                t->host_thread = CreateThread(NULL, spu_host_stack_bytes(),
                                              spu_exec_thread_proc, t,
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#else
                pthread_mutex_init(&t->finish_event.mu, NULL);
                pthread_cond_init(&t->finish_event.cv, NULL);
                t->finish_event.done = 0;
                spu_spawn_host_thread(&t->host_thread, spu_exec_thread_proc, t);
#endif
                spawned++;
                continue;
            }
        }

        void* user = NULL;
        spu_ppu_fallback_fn fb = spu_lookup_ppu_fallback(t->entry_point, &user);
        /* No fallback by entry point -- ask the WORKLOAD registry, which is
         * keyed by the image's content fingerprint and is what
         * build_spu_workloads.py populates. The two registries have always both
         * existed; only the SPURS path consulted this one, so a title driving
         * plain SPU thread groups never ran a line of its lifted SPU code. */
        if (!fb && t->img_ea && vm_base) {
            extern int32_t spu_registry_fallback(uint32_t, uint32_t, uint32_t, void*);
            uint32_t src = t->img_src;
            size_t isz = src ? spu_elf_image_size(vm_base + src, 1u << 20) : 0;
            if (isz) {
                uint64_t fp = spu_workload_fingerprint(vm_base + src, isz);
                int iid = 0;
                if (spu_workload_find_img(fp, &iid)) {
                    fb = spu_registry_fallback;
                    user = (void*)(uintptr_t)fp;
                    fprintf(stderr, "[SPU] thread tid=0x%X image @0x%08X (%u bytes) "
                            "matched lifted workload fp=0x%016llX image_id=%d\n",
                            t->tid, src, (unsigned)isz,
                            (unsigned long long)fp, iid);
                    /* Load the image into this thread's local store.
                     *
                     * Lifting supplies the INSTRUCTIONS, not the data: .data,
                     * .rodata, jump tables and the initial stack area all live
                     * in LS. The SPURS/workload dispatch paths call
                     * spu_elf_load_to_ls before running a job, but the raw
                     * sys_spu_thread_* path never did, and a title that starts a
                     * plain SPU thread group does not write LS itself. So the
                     * worker ran against 256 KB of zeroes: MultiStream's mixer
                     * managed ~12 lifted hops, touched no channel at all, and
                     * returned (branch to LS 0) without reaching its service
                     * loop.
                     *
                     * Once only, at group_start: re-running a parked worker
                     * (sys_spu_thread_write_spu_mb) must keep the local store it
                     * has built up, not reset it. */
                    { extern int spu_elf_load_to_ls(const uint8_t*, size_t,
                                                    uint8_t*, uint32_t*);
                      uint8_t* ls = spu_thread_get_local_store(t->tid);
                      uint32_t ls_entry = 0;
                      if (ls && spu_elf_load_to_ls(vm_base + src, isz, ls, &ls_entry))
                          fprintf(stderr, "[SPU] thread tid=0x%X local store loaded "
                                  "(entry 0x%05X)\n", t->tid, ls_entry);
                      else
                          fprintf(stderr, "[SPU] thread tid=0x%X LOCAL STORE LOAD FAILED "
                                  "-- worker will run against zeroes\n", t->tid);
                    }
                } else {
                    fprintf(stderr, "[SPU] thread tid=0x%X image @0x%08X (%u bytes) "
                            "fp=0x%016llX is NOT in the workload registry\n",
                            t->tid, src, (unsigned)isz, (unsigned long long)fp);
                }
            }
        }
        if (!fb && t->img_ea) {
            /* No lifted code: the image runs on the interpreter. (It used to
             * "complete" instantly with status 0 unless RD_SPU_INTERP was set --
             * an SPU thread that never ran, reported as successful.) */
            fb = spu_interp_fallback;
            user = NULL;
        }
        if (!fb) {
            t->exit_status = 0;
            __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
            instant++; nofb++;
            continue;
        }
        t->fb_handler = fb;
        t->fb_user    = user;
        __atomic_store_n(&t->running, 1, __ATOMIC_RELEASE);
        /* Interpreted sim jobs are fire-and-forget compute (DMA in -> compute ->
         * DMA out -> stop) that don't block on PPU input mid-run. Running them on
         * an async host thread races the PPU's own use of the results (e.g. the
         * ducky's initShaders aborts nondeterministically). Run them SYNCHRONOUSLY
         * here so group_start returns only after the SPU has finished and written
         * its output -- deterministic, and matches how the PPU expects to consume
         * the results right after start/join. (RD_SPU_INTERP_ASYNC forces the old
         * async path if a job ever needs to overlap with the PPU.) */
        /* Raw persistent workers run synchronously too. park_on_empty_inmbox
         * exists so such a worker does its full init + ready handshake and then
         * PARKS at its first idle mailbox poll -- the whole point being that no
         * async host thread races the PPU. Spawning one anyway meant the
         * group_start run and a write_spu_mb re-run competed for the same
         * mailbox word: one run consumed it, the other polled an empty box and
         * parked, and which got it varied run to run. */
        if (fb == spu_interp_fallback && !getenv("RD_SPU_INTERP_ASYNC")) {
            extern SPU_THREAD_LOCAL int g_spu_interp_group_exit_valid;
            extern SPU_THREAD_LOCAL int32_t g_spu_interp_group_exit_status;
            t->exit_status = fb(t->tid, t->args_ea, t->args_size, user);
            __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
            if (g_spu_interp_group_exit_valid) {
                AcquireSRWLockExclusive(&s_grp_lock);
                if (g->state == SPU_GROUP_STATE_RUNNING && g->run_gen == gen) {
                    g->cause       = SPU_GROUP_CAUSE_GROUP_EXIT;
                    g->exit_status = g_spu_interp_group_exit_status;
                }
                ReleaseSRWLockExclusive(&s_grp_lock);
            }
            instant++;
            continue;
        }
        t->run_gen = gen;
        t->host_live = 1;
        AcquireSRWLockExclusive(&s_grp_lock); g->running++; ReleaseSRWLockExclusive(&s_grp_lock);
#ifdef _WIN32
        /* Manual-reset event so multiple group_join callers all see "set" */
        if (!t->finish_event)
            t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        else
            ResetEvent(t->finish_event);
        t->host_thread = CreateThread(NULL, spu_host_stack_bytes(),
                                      spu_fallback_thread_proc, t,
                                      STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#else
        pthread_mutex_init(&t->finish_event.mu, NULL);
        pthread_cond_init(&t->finish_event.cv, NULL);
        t->finish_event.done = 0;
        spu_spawn_host_thread(&t->host_thread, spu_fallback_thread_proc, t);
#endif
        /* Start handshake for a persistent worker: do not let group_start
         * return until the worker has published its live context.
         *
         * Without this the PPU can call sys_spu_thread_write_spu_mb before the
         * freshly spawned host thread has reached spu_thread_publish_ctx, see a
         * null live_ctx, and take the re-run-from-entry fallback -- which
         * restarts init and consumes the command as a startup parameter. The
         * race is genuinely tight: it only stayed hidden while the mailbox path
         * was logging every write, because the fprintf/fflush was slowing the
         * PPU down enough for the worker to win. spu_raw.c gates on a `started`
         * flag for the same reason.
         *
         * Bounded, so a worker that dies during init cannot hang group_start. */
        if (fb == spu_registry_fallback || fb == spu_interp_fallback) {
            for (int spin = 0; spin < 2000 && !__atomic_load_n(&t->live_ctx, __ATOMIC_ACQUIRE) &&
                               __atomic_load_n(&t->running, __ATOMIC_ACQUIRE); spin++)
#ifdef _WIN32
                Sleep(1);
#else
                { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
#endif
            if (!__atomic_load_n(&t->live_ctx, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&t->running, __ATOMIC_ACQUIRE))
                fprintf(stderr, "[SPU] group_start tid=0x%X: worker never published "
                        "a context -- mailbox writes will fall back to re-runs\n",
                        t->tid);
        }
        fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X args=0x%08X -> spawned host thread\n",
                id, t->tid, t->entry_point, t->args_ea);
        spawned++;
    }

    /* Drop the guard; if every thread already finished (or none was
     * launched), the run ends here. */
    AcquireSRWLockExclusive(&s_grp_lock);
    if (g->state == SPU_GROUP_STATE_RUNNING && g->run_gen == gen && --g->running == 0)
        spu_group_run_end_locked(g);
    ReleaseSRWLockExclusive(&s_grp_lock);
    if (spawned == 0) {
        /* `instant` counts BOTH no-fallback threads and threads that ran to
         * completion synchronously (the interpreter path). Reporting "no
         * fallback" whenever spawned==0 hid a perfectly working interpreted
         * run -- say which it actually was. */
        fprintf(stderr, "[SPU] group_start id=0x%X (%u thread(s), none spawned: %d ran synchronously, %d had no fallback)\n",
                id, g->num_threads, instant - nofb, nofb);
    } else {
        fprintf(stderr, "[SPU] group_start id=0x%X (%d host threads running, %d instant)\n",
                id, spawned, instant);
    }
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_join(id, *cause, *status)
 *
 * As lv2: ESTAT before every thread is initialized; EBUSY while another
 * thread is joining; a run that ended and was not joined yet is returned at
 * once (and only once); otherwise the caller sleeps until the current -- or,
 * for a group that is not running, the next -- run ends. */
static int64_t sys_spu_thread_group_join_handler(ppu_context* ctx)
{
    uint32_t id         = (uint32_t)ctx->gpr[3];
    uint32_t cause_ea   = (uint32_t)ctx->gpr[4];
    uint32_t status_ea  = (uint32_t)ctx->gpr[5];

    AcquireSRWLockExclusive(&s_grp_lock);
    spu_group_t* g = spu_find_group(id);
    if (!g) { ReleaseSRWLockExclusive(&s_grp_lock); LV2_RET(ctx, CELL_ESRCH); }
    if (g->state == SPU_GROUP_STATE_NOT_INITIALIZED) {
        ReleaseSRWLockExclusive(&s_grp_lock);
        LV2_RET(ctx, CELL_ESTAT);
    }
    if (g->has_waiter) { ReleaseSRWLockExclusive(&s_grp_lock); LV2_RET(ctx, CELL_EBUSY); }
    uint32_t cause;
    int32_t  status;
    if (g->state == SPU_GROUP_STATE_INITIALIZED && g->join_state) {
        cause  = g->join_state;
        status = g->exit_status;
        g->join_state = 0;
    } else {
        g->has_waiter  = 1;
        g->waiter_done = 0;
        while (!g->waiter_done)
            SleepConditionVariableSRW(&s_grp_cv, &s_grp_lock, INFINITE, 0);
        g->has_waiter = 0;
        cause  = g->waiter_cause;
        status = g->waiter_status;
    }
    ReleaseSRWLockExclusive(&s_grp_lock);
    spu_group_reap(g);

    if (cause_ea)  vm_write_be32(cause_ea,  cause);
    if (status_ea) vm_write_be32(status_ea, (uint32_t)status);
    fprintf(stderr, "[SPU] group_join id=0x%X cause=%u status=%d (event_queue=0x%X)\n",
            id, cause, status, g->event_queue_id);
    fflush(stderr);
    LV2_RET(ctx, CELL_OK);
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
        fprintf(stderr, "[SPU] group_destroy id=0x%X SKIPPED (YDKJ_KEEPGROUP) caller_lr=0x%08X cia=0x%08X r1=0x%08X r2=0x%08X\n",
                id, (uint32_t)ctx->lr, (uint32_t)ctx->cia, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[2]);
        { extern void ppu_dump_guest_stack(ppu_context*, const char*); ppu_dump_guest_stack(ctx, "group_destroy-caller"); }
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }
    spu_group_t* g = spu_find_group(id);
    if (!g) LV2_RET(ctx, CELL_ESRCH);
    /* EBUSY while a run is in progress (lv2: state above INITIALIZED). */
    if (g->state == SPU_GROUP_STATE_RUNNING) LV2_RET(ctx, CELL_EBUSY);
    /* A run that ended by group exit or terminate can leave a thread still
     * finishing on its host thread; it must be gone before its context is. */
    int stopped;
    AcquireSRWLockExclusive(&s_grp_lock);
    spu_group_request_stop_locked(g);
    stopped = spu_group_wait_stopped_locked(g, 5000);
    ReleaseSRWLockExclusive(&s_grp_lock);
    spu_group_reap(g);
    {
        for (int i = 0; i < 8; i++) {
            uint32_t idx = g->thread_indices[i];
            if ((g->init_mask & (1u << i)) && idx < MAX_SPU_THREADS) {
                spu_thread_t* t = &s_spu_threads[idx];
                if (t->local_store) {
                    free(t->local_store);
                    t->local_store = NULL;
                }
                if (t->sctx && !stopped && __atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) {
                    fprintf(stderr, "[SPU] group_destroy id=0x%X: tid=0x%X still running after "
                            "5 s -- leaking its context rather than freeing it under it\n", id, t->tid);
                    t->sctx = NULL;
                } else if (t->sctx) {
                    /* Out of every table keyed by the context's address first:
                     * the coherence registry would otherwise notify freed
                     * memory (mcx X5 under ThreadSanitizer: SEGV in
                     * notify_spus), and the MFC slot registry would hand the
                     * dead context's queue and tag state to the next context
                     * calloc places at the same address. */
                    extern void spu_coh_unregister(spu_context*);
                    extern void spu_mfc_release(spu_context*);
                    spu_coh_unregister(t->sctx);
                    spu_mfc_release(t->sctx);
                    free(t->sctx);
                    t->sctx = NULL;
                }
                SLOT_FREE(t);
            }
        }
        SLOT_FREE(g);
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
    AcquireSRWLockExclusive(&s_grp_lock);
    spu_group_t* g = spu_find_group(id);
    if (!g) { ReleaseSRWLockExclusive(&s_grp_lock); LV2_RET(ctx, CELL_ESRCH); }
    if (g->state != SPU_GROUP_STATE_RUNNING) {
        ReleaseSRWLockExclusive(&s_grp_lock);
        LV2_RET(ctx, CELL_ESTAT);
    }
    /* lv2 stops the group's SPUs before this returns. Ask every running
     * thread's context to stop (each checks at its next channel wait,
     * interpreter step or lifted trampoline), wake the ones parked in a
     * channel read, and wait for the last of them to end the run itself --
     * so group_join and group_destroy never race a host thread still
     * executing on the context they are about to free (mcx suite, X5). */
    g->cause       = SPU_GROUP_CAUSE_TERMINATED;
    g->exit_status = status;
    const uint32_t gen = g->run_gen;
    const int asked = spu_group_request_stop_locked(g);
    int ms = 0;
    while (asked && g->state == SPU_GROUP_STATE_RUNNING && g->run_gen == gen && ms < 5000) {
        ReleaseSRWLockExclusive(&s_grp_lock);
#ifdef _WIN32
        Sleep(1);
#else
        { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
#endif
        ms++;
        AcquireSRWLockExclusive(&s_grp_lock);
    }
    if (g->state == SPU_GROUP_STATE_RUNNING && g->run_gen == gen) {
        if (asked)
            fprintf(stderr, "[SPU] group_terminate id=0x%X: a thread did not stop within 5 s "
                    "(a loop with no channel access or branch the runtime can see) -- "
                    "ending the run with it still executing\n", id);
        spu_group_run_end_locked(g);
    }
    ReleaseSRWLockExclusive(&s_grp_lock);
    fprintf(stderr, "[SPU] group_terminate id=0x%X status=%d (%d thread(s) stopped)\n",
            id, status, asked);
    fflush(stderr);
    LV2_RET(ctx, CELL_OK);
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
    if (__atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) {
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
    if (t) {
        t->args_ea = arg_ea;
        for (int a = 0; a < 4; a++) {     /* lv2 copy semantics, as above */
            uint64_t hi = arg_ea ? vm_read_be32(arg_ea + (uint32_t)a * 8)     : 0;
            uint64_t lo = arg_ea ? vm_read_be32(arg_ea + (uint32_t)a * 8 + 4) : 0;
            t->args[a] = (hi << 32) | lo;
        }
    }

    fprintf(stderr, "[SPU] thread_set_argument tid=0x%X arg=0x%08X\n",
            tid, arg_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_write_spu_mb(spu_thread_id, value) -- PPU -> SPU inbound mailbox.
 *
 * This was the ONLY SPU-thread syscall left unregistered, so it fell through to
 * the generic stub and the word was silently dropped. The outbound (SPU -> PPU)
 * direction was already implemented, which made the gap easy to miss: everything
 * looked wired up until a title actually pushed a command.
 *
 * A raw SPU thread is a persistent worker. Ours does not stay resident between
 * commands -- it parks (halts) at its idle mailbox poll and its local store is
 * saved -- so "write the mailbox" is delivered by re-running the worker with the
 * word pre-loaded, which is the same shape the interpreter path already uses for
 * per-frame work descriptors. Run it SYNCHRONOUSLY: the caller's very next move
 * is normally sys_event_queue_receive on the queue this worker replies to, and a
 * host thread racing that is how completion events get lost.
 *
 * Rampage World Tour hands its MultiStream mixer work exactly this way, then
 * blocks on event queue 2 for the reply.
 *
 * ponytail: re-runs the worker from its ENTRY rather than resuming where it
 * parked -- local store persists, registers do not. Fine for a worker whose init
 * is idempotent (MultiStream's is). A worker that carries live state in
 * registers across an idle-park would need the context saved at the park and
 * restored here instead. */
static int64_t sys_spu_thread_write_spu_mb_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t val = (uint32_t)ctx->gpr[4];

    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        /* Not the guest's error -- ours. Our SPU threads are not resident:
         * a group whose work runs on the interpreter reports "1 ran
         * synchronously" and is gone by the time the PPU writes to it, so a
         * lookup the title is entitled to expect to succeed fails here.
         *
         * Before this syscall was implemented it fell through to the generic
         * stub, which returns CELL_OK, and every title carried on. A hard
         * failure instead turns our gap into the title's crash: Rubber
         * Ducky's spu_printf_handler calls this with the printf port 0x3F,
         * gets the error, and calls sys_ppu_thread_exit -- taking the scene
         * load, the shaders and every draw with it. It rendered a complete
         * bathroom before this landed and drew nothing after.
         *
         * So keep the contract every port was built against, and be loud
         * about it rather than silent.
         * ponytail: reports success for a word it could not deliver. The
         * real fix is resident SPU threads, or a printf port that resolves
         * to its thread; this is the floor until one of those exists. */
        static int warned = 0;
        if (__atomic_fetch_add(&warned, 1, __ATOMIC_RELAXED) < 8) {
            fprintf(stderr, "[SPU] write_spu_mb: thread 0x%X not found -- "
                            "dropping the word and reporting CELL_OK, as the "
                            "unimplemented stub did\n", tid);
            fflush(stderr);
        }
        ctx->gpr[3] = CELL_OK;
        return CELL_OK;
    }

    /* Preferred path: the worker is alive and blocked in rdch on its own host
     * thread. Write its mailbox and wake it, so it resumes exactly where it was
     * with its registers intact. */
    AcquireSRWLockShared(&s_live_ctx_lock);
    if (t->live_ctx || t->sctx) {
        extern void spu_ch_wake(spu_context* c);
        spu_context* c = t->live_ctx ? (spu_context*)t->live_ctx : t->sctx;

        /* The inbound mailbox is four deep, and a write to a full one replaces
         * its newest entry: it neither fails nor waits (RPCS3 and the CBEA
         * agree; tests/conformance/mc mcx X2). Earlier versions made it one
         * deep and blocked the writer up to 250 ms per word, then failed with
         * CELL_EBUSY -- a title that writes a multi-word command in one go
         * saw its second word refused. */
        { static int _n = 0;
          if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 32)
              fprintf(stderr, "[SPU] write_spu_mb tid=0x%X val=0x%08X -> live worker\n",
                      tid, val); }
        spu_channel_push_inmbox(&c->ch_in_mbox, val);
        spu_ch_wake(c);
        ReleaseSRWLockShared(&s_live_ctx_lock);
        ctx->gpr[3] = 0;
        return 0;
    }
    ReleaseSRWLockShared(&s_live_ctx_lock);

    /* Fallback: no run in flight (the worker finished). Queue the word and
     * re-run it from its entry with the value pre-loaded. */
    t->pending_inmbox = val;
    fprintf(stderr, "[SPU] write_spu_mb tid=0x%X val=0x%08X%s\n", tid, val,
            t->fb_handler ? " -> re-running worker" : " (no fallback: queued only)");
    fflush(stderr);

    if (t->fb_handler)
        t->exit_status = t->fb_handler(t->tid, t->args_ea, t->args_size, t->fb_user);

    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_connect_event(group_id, queue_id, event_type)
 *
 * Bind a lifecycle SYS_EVENT queue to the group; we record queue_id so
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
    if ((uint32_t)ctx->gpr[5] == 1) g->run_queue_id = queue_id;   /* SYS_SPU_THREAD_GROUP_EVENT_RUN */
    fprintf(stderr, "[SPU] group_connect_event group=0x%X queue=0x%X et=%u\n",
            group_id, queue_id, (uint32_t)ctx->gpr[5]);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* User-event ports are independent of group lifecycle event connections. */
static SRWLOCK s_spu_port_lock = SRWLOCK_INIT;
/* sys_spu_thread_group_disconnect_event_all_threads(id, spup) -- 252 */
static int64_t sys_spu_thread_group_disconnect_event_all_threads_handler(ppu_context* ctx)
{
    const uint32_t port = (uint32_t)ctx->gpr[4];
    if (port > 63) LV2_RET(ctx, CELL_EINVAL);
    spu_group_t* g = spu_find_group((uint32_t)ctx->gpr[3]);
    if (!g) LV2_RET(ctx, CELL_ESRCH);
    AcquireSRWLockExclusive(&s_spu_port_lock);
    g->user_event_ports[port] = 0;
    ReleaseSRWLockExclusive(&s_spu_port_lock);
    LV2_RET(ctx, CELL_OK);
}

static int64_t sys_spu_thread_group_connect_event_all_threads_handler(ppu_context* ctx)
{
    extern int sys_event_queue_exists(uint32_t queue_id);
    spu_group_t* g = spu_find_group((uint32_t)ctx->gpr[3]);
    uint64_t requested = ctx->gpr[5];
    uint32_t output = (uint32_t)ctx->gpr[6];
    uint32_t result = CELL_OK;
    if (!requested) result = CELL_EINVAL;
    else if (!g || !sys_event_queue_exists((uint32_t)ctx->gpr[4])) result = CELL_ESRCH;
    else if (g->state == SPU_GROUP_STATE_NOT_INITIALIZED) result = CELL_ESTAT;
    else if (!output) result = CELL_EFAULT;
    else {
        result = CELL_EISCONN;
        AcquireSRWLockExclusive(&s_spu_port_lock);
        for (unsigned port = 0; port < 64; ++port) {
            if ((requested & (1ull << port)) && !g->user_event_ports[port]) {
                g->user_event_ports[port] = (uint32_t)ctx->gpr[4];
                gm_store8(vm_base + output, (uint8_t)port);
                result = CELL_OK;
                break;
            }
        }
        ReleaseSRWLockExclusive(&s_spu_port_lock);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)result;
    return (int64_t)(int32_t)result;
}

/* The interrupt mailbox encodes send_event (0..63) or throw_event (64..127).
 * The preceding ordinary mailbox word is data, not a separate PPU event. */
static int spu_deliver_user_event(spu_context* spu, uint32_t value)
{
    unsigned code = value >> 24;
    /* No lv2 group: SPU code under SPURS. Only its user events route, through
     * the ports cellSpursAttachLv2EventQueue bound; the rest stays as it was. */
    extern uint32_t spurs_port_queue(uint32_t port);
    if (!spu->spu_group_id && (code >= 128 || !spurs_port_queue(code & 63))) {
        if (code < 128) { static int n; if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 16)
            fprintf(stderr, "[spu-evt] SPURS user event on unbound port %u dropped (img=%d value=0x%08X)\n",
                    code & 63, spu->image_id, value); }
        return 0;
    }
    /* Task-exit handlers signal an LV2 flag, not an SPU user-event queue.
     * 128 acknowledges the result; 192 is the impatient, no-ack form. */
    if (code == 128 || code == 192) {
        uint32_t result = CELL_EINVAL;
        if (spu_channel_count(&spu->ch_out_mbox)) {
            uint32_t flag_id = spu_channel_read(&spu->ch_out_mbox);
            uint32_t bit = value & 0xFFFFFFu;
            if (bit < 64) {
                ppu_context call = {0};
                call.gpr[3] = flag_id;
                call.gpr[4] = 1ull << bit;
                result = (uint32_t)sys_event_flag_set(&call);
            }
        }
        if (code == 128) spu_channel_write(&spu->ch_in_mbox, result);
        return 1;
    }
    if (code >= 128) return 0;
    uint32_t result = CELL_EINVAL;
    if (spu_channel_count(&spu->ch_out_mbox)) {
        uint32_t data = spu_channel_read(&spu->ch_out_mbox);
        unsigned port = code & 63;
        uint32_t queue = 0;
        if (!spu->spu_group_id) {
            queue = spurs_port_queue(port);
        } else {
            AcquireSRWLockShared(&s_spu_port_lock);
            spu_group_t* group = spu_find_group(spu->spu_group_id);
            if (group) queue = group->user_event_ports[port];
            ReleaseSRWLockShared(&s_spu_port_lock);
            /* A port bound to this one thread by sys_spu_thread_connect_event
             * (SYS_SPU_THREAD_EVENT_USER) rather than group-wide. */
            if (!queue) {
                spu_thread_t* t = spu_find_thread(spu->spu_id);
                if (t) for (int i = 0; i < t->evt_bind_n; i++)
                    if (t->evt_bind[i].spup == port) { queue = t->evt_bind[i].queue; break; }
            }
        }
        result = CELL_ENOTCONN;
        if (queue) {
            int rc = sys_event_queue_push_by_id(queue, 0xFFFFFFFF53505501ull,
                spu->spu_id, ((uint64_t)port << 32) | (value & 0xFFFFFFu), data);
            result = rc == 0 ? CELL_OK : CELL_EBUSY;
        }
    }
    if (code < 64) spu_channel_write(&spu->ch_in_mbox, result);
    return 1;
}

static int64_t sys_spu_thread_group_disconnect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(group_id);
    if (g) { g->event_queue_id = 0; if ((uint32_t)ctx->gpr[4] == 1) g->run_queue_id = 0; }
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
    uint32_t spup = (uint32_t)ctx->gpr[6];
    if (t) {
        /* First binding stays the default for anything we cannot port-route. */
        if (!t->connected_queue) { t->connected_queue = eq; t->connect_spup = spup; }
        int slot = -1;
        for (int i = 0; i < t->evt_bind_n; i++)
            if (t->evt_bind[i].spup == spup) { slot = i; break; }
        if (slot < 0 && t->evt_bind_n < 8) slot = t->evt_bind_n++;
        if (slot >= 0) { t->evt_bind[slot].spup = spup; t->evt_bind[slot].queue = eq; }
    }
    fprintf(stderr, "[SPU] thread_connect_event tid=0x%X queue=0x%X et=0x%X spup=0x%X%s\n",
            tid, eq, et, (uint32_t)ctx->gpr[6], t ? "" : " (thread not found)");
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_bind_queue(id, spuq, spuq_num) / unbind_queue(id, spuq_num).
 * The SPU later names spuq_num in sys_spu_thread_receive_event; these were
 * stubs, so the SPU could never receive anything the PPU sent it (inFamous'
 * Bink movie SPU polled tryreceive forever and the splash never played). */
static int64_t sys_spu_thread_bind_queue_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t eq  = (uint32_t)ctx->gpr[4];
    uint32_t num = (uint32_t)ctx->gpr[5];
    spu_thread_t* t = spu_find_thread(tid);
    int32_t rc = CELL_OK;
    if (!t) rc = (int32_t)0x80010005;                 /* ESRCH */
    else {
        int slot = -1;
        for (int i = 0; i < t->q_bind_n; i++)
            if (t->q_bind[i].num == num) { slot = i; break; }
        if (slot >= 0) rc = (int32_t)0x80010014;      /* EBUSY: number in use */
        else if (t->q_bind_n >= 16) rc = (int32_t)0x80010004; /* ENOMEM */
        else { t->q_bind[t->q_bind_n].num = num; t->q_bind[t->q_bind_n].queue = eq; t->q_bind_n++; }
    }
    fprintf(stderr, "[SPU] thread_bind_queue tid=0x%X queue=0x%X spuq_num=0x%X -> 0x%X\n",
            tid, eq, num, (uint32_t)rc);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

static int64_t sys_spu_thread_unbind_queue_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t num = (uint32_t)ctx->gpr[4];
    spu_thread_t* t = spu_find_thread(tid);
    int32_t rc = (int32_t)0x80010005;                 /* ESRCH */
    if (t) {
        for (int i = 0; i < t->q_bind_n; i++)
            if (t->q_bind[i].num == num) {
                t->q_bind[i] = t->q_bind[--t->q_bind_n];
                rc = CELL_OK;
                break;
            }
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* lv2 stop-and-signal service for lifted SPU threads (installed as
 * g_spu_lv2_stop_hook). Returns 1 when the stop was a syscall serviced here,
 * so the SPU resumes at the next instruction:
 *   0x100 yield            -- nothing to do but resume
 *   0x110 receive_event    -- out mbox = spuq_num; reply {rc, d1, d2, d3}
 *   0x111 tryreceive_event -- same, but EBUSY instead of blocking
 * The reply goes through the context's rcv_evt words, which the inbound
 * mailbox hands out before anything else. On an error only rc is sent, which
 * is what the SPU-side wrapper reads before it gives up. */
extern int32_t sys_event_queue_pop_internal(uint32_t, int, sys_event_t*);
static int spu_lv2_stop_service(spu_context* spu)
{
    if (!spu->spu_group_id) return 0;                 /* SPURS, not an lv2 thread */
    if (spu->stop_code == 0x100) return 1;
    if (spu->stop_code != 0x110 && spu->stop_code != 0x111) return 0;
    spu_thread_t* t = spu_find_thread(spu->spu_id);
    if (!t) return 0;
    uint32_t num = spu_channel_count(&spu->ch_out_mbox) ? spu_channel_read(&spu->ch_out_mbox) : 0xFFFFFFFFu;
    uint32_t queue = 0;
    for (int i = 0; i < t->q_bind_n; i++)
        if (t->q_bind[i].num == num) { queue = t->q_bind[i].queue; break; }
    sys_event_t ev;
    int32_t rc = queue ? sys_event_queue_pop_internal(queue, spu->stop_code == 0x110, &ev)
                       : (int32_t)0x80010002;         /* EINVAL: nothing bound */
    spu->rcv_evt[0] = (uint32_t)rc;
    if (rc == CELL_OK) {
        spu->rcv_evt[1] = (uint32_t)ev.data1;
        spu->rcv_evt[2] = (uint32_t)ev.data2;
        spu->rcv_evt[3] = (uint32_t)ev.data3;
        spu->rcv_evt_n = 4;
    } else {
        spu->rcv_evt_n = 1;
    }
    spu->rcv_evt_i = 0;
    { static int n = 0; if (rc == CELL_OK && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 16)
        fprintf(stderr, "[SPU] tid=0x%X receive_event spuq=0x%X q=%u -> d1=0x%llX d2=0x%llX d3=0x%llX\n",
                spu->spu_id, num, queue, (unsigned long long)ev.data1,
                (unsigned long long)ev.data2, (unsigned long long)ev.data3); }
    return 1;
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
    /* lv2 encodes the destination SPU PORT in the top byte of the word an SPU
     * sends to the PPU: MultiStream's completion word 0x2A000001 is port 0x2A,
     * which is the queue it bound with connect_event(..., spup=0x2A). Route on
     * that; only fall back to the default binding when the port is unknown (a
     * plain out-mailbox value is PPU-polled, not port-addressed). */
    if (t) {
        uint32_t port = (value >> 24) & 0xFF;
        for (int i = 0; i < t->evt_bind_n; i++)
            if (t->evt_bind[i].spup == port) { q = t->evt_bind[i].queue; break; }
    }
    if (!q && t && t->connected_queue) q = t->connected_queue;
    if (!q) { spu_group_t* g = spu_find_group(group_id); if (g) q = g->event_queue_id; }
    { static int s_d = 0; if (getenv("SPU_MBOXTRACE") && __atomic_fetch_add(&s_d, 1, __ATOMIC_RELAXED) < 64)
        fprintf(stderr, "[SPU->PPU] deliver? spu=0x%X intr=%d val=0x%08X q=%u (thread %s)\n",
                spu_id, is_intr, value, q, t ? "found" : "MISSING"); }
    if (!q) return;
    /* SPURS SPU-event source convention: high word tags it as an SPU thread
     * event; data1 = the mailbox value. */
    sys_event_queue_push_by_id(q,
        ((uint64_t)spu_id << 32) | (is_intr ? 0x2u : 0x1u),
        (uint64_t)value, 0, 0);
    { static int s_w = 0; if (__atomic_fetch_add(&s_w, 1, __ATOMIC_RELAXED) < 32)
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
    /* A thread running lifted code owns its local store inside the SPU context,
     * and that is the store the SPU code reads and writes. Hand back the same
     * 256 KB so sys_spu_thread_read_ls sees what the SPU actually wrote instead
     * of a second, empty buffer. Nothing frees this one: the context owns it. */
    if (t->sctx) return t->sctx->ls;
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
    { static _Atomic int s_t = -1; if (s_t < 0) s_t = getenv("SPU_LSREAD_TRACE") ? 1 : 0;
      static int n = 0;
      if (s_t && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 12)
          { extern void ppu_guest_caller(char*, size_t);
            char who[64]; ppu_guest_caller(who, sizeof who);
            fprintf(stderr, "[spu-readls] tid=0x%08X off=0x%05X size=%u from %s\n",
                    tid, ls_offset, type, who); } }
    /* A SPURS job chain is polled by its CHAIN HANDLE, not an lv2 thread id.
     * This is the SPU PRINTF service: it reads a pointer from local store and
     * then walks a format string byte by byte (func_00250A8C). Without this
     * the lookup fails outright and the title reports
     * "failed to SPURS printf server". It is debug output, not the path any
     * query result travels. */
    { extern const uint8_t* spurs_job_ls_for_handle(uint32_t);
      const uint8_t* jls = spurs_job_ls_for_handle(tid);
      if (jls && value_ea && vm_base && ls_offset + type <= SPU_LS_SIZE) {
          uint64_t v = 0;
          for (uint32_t k = 0; k < type && k < 8; k++)
              v = (v << 8) | jls[ls_offset + k];
          gm_store64(vm_base + value_ea, __builtin_bswap64(v));
          { static _Atomic int s_t = -1; if (s_t < 0) s_t = getenv("SPU_LSREAD_TRACE") ? 1 : 0;
            static int n = 0;
            if (s_t && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 8)
                fprintf(stderr, "[spu-readls] chain 0x%08X off=0x%05X -> 0x%llX\n",
                        tid, ls_offset, (unsigned long long)v); }
          ctx->gpr[3] = 0;
          return 0;
      } }
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

/* Public: parent group of a SPU thread (0 if unknown). The lifted-run options
 * carry it so an outbound mailbox word can be routed back to the right queue. */
uint32_t spu_thread_get_group_id(uint32_t tid)
{
    spu_thread_t* t = spu_find_thread(tid);
    return t ? t->group_id : 0;
}

/* Published by the lifted runner for the lifetime of a worker's run. */
void spu_thread_publish_ctx(uint32_t tid, void* c)
{
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) return;
    AcquireSRWLockExclusive(&s_live_ctx_lock);
    __atomic_store_n(&t->live_ctx, c, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&s_live_ctx_lock);
}

/* Public: consume the pending inbound-mailbox command for a SPU thread, if any.
 * Read-and-clear: one PPU write is delivered to exactly one SPU run. */
uint32_t spu_thread_take_pending_inmbox(uint32_t tid)
{
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) return 0;
    uint32_t v = t->pending_inmbox;
    t->pending_inmbox = 0;
    return v;
}

/* The kernel's SPU image objects: _sys_spu_image_import (157), which takes a
 * copy of an SPU ELF for liblv2's PROTECT import, _sys_spu_image_close (158)
 * and _sys_spu_image_get_segments (159). The work is lv2_spu_image.c; liblv2's
 * sys_spu_image_import / close, which call these, are in ppu_sysprx.cpp. */
static int64_t sys_spu_image_import_handler(ppu_context* ctx)
{
    LV2_RET(ctx, lv2_spu_image_import((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                                      (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]));
}

static int64_t sys_spu_image_close_handler(ppu_context* ctx)
{
    LV2_RET(ctx, lv2_spu_image_close((uint32_t)ctx->gpr[3]));
}

static int64_t sys_spu_image_get_segments_handler(ppu_context* ctx)
{
    LV2_RET(ctx, lv2_spu_image_get_segments((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                                            (int32_t)ctx->gpr[5]));
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

    /* Read the WHOLE image into guest memory and hand it to the import path.
     *
     * This used to read 52 bytes, report e_entry and stop, on the reasoning that
     * lifted SPU code does not execute out of local store so the segments need
     * not be materialised. Two things actually do depend on them:
     *
     *   - the raw-SPU layer resolves an image to its lifted entry by
     *     FINGERPRINTING THE ELF BYTES, and only _sys_spu_image_import ever did
     *     that. A title loading raw SPU images by path never reached the
     *     registry, so a correctly registered image was never found and the SPU
     *     never started -- while the PPU span forever on SPU_Mbox_Stat waiting
     *     for a message from it.
     *   - lifted SPU code still READS local store for its own constants and
     *     data. An image whose descriptor carries zero segments comes up in
     *     4096 zeroed lines and halts at LS 0, which looks exactly like a
     *     lifting bug and is not one.
     *
     * An image named by path is an image imported from memory once the file has
     * been read in: same ELF, same segment table, same fingerprint. So stage the
     * bytes and call sys_spu_image_import_handler rather than re-deriving any of
     * it here -- re-deriving is how the two paths drifted apart to begin with.
     */
    long fsz = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (fsz <= 0 || fsz > (long)SPU_IMAGE_STAGE_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' — implausible size %ld\n",
                img_ea, ps3_path, fsz);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }

    /* Guest scratch for the staged ELF. It shares the 16 MB region below the TLS
     * block with the segment tables, which grow up from 0x0D000000; images grow
     * up from the halfway mark. Reaching the 8 MB that would let them meet needs
     * ~350k segment records, which no title comes close to. */
    static uint32_t s_spu_img_bump = SPU_IMAGE_STAGE_BASE;
    if (s_spu_img_bump + (uint32_t)fsz > SPU_IMAGE_STAGE_END)
        s_spu_img_bump = SPU_IMAGE_STAGE_BASE;                 /* wrap */
    uint32_t staged_ea = s_spu_img_bump;

    /* Read into a host buffer and copy, rather than fread-ing straight into the
     * guest map: a short read then cannot leave half an image staged for the
     * import path to parse as a whole one. */
    uint8_t* buf = (uint8_t*)malloc((size_t)fsz);
    size_t got = buf ? fread(buf, 1, (size_t)fsz, f) : 0;
    fclose(f);
    if (got != (size_t)fsz) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' — short read "
                        "(%zu of %ld)\n", img_ea, ps3_path, got, fsz);
        fflush(stderr);
        free(buf);
        ctx->gpr[3] = 0;
        return 0;
    }
    memcpy(vm_base + staged_ea, buf, (size_t)fsz);
    free(buf);
    s_spu_img_bump += ((uint32_t)fsz + 0x7F) & ~0x7Fu;          /* keep it aligned */

    fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' staged %ld bytes at "
                    "0x%08X\n", img_ea, ps3_path, fsz, staged_ea);
    fflush(stderr);

    /* Re-enter as an import of the staged bytes. gpr[4] is the source operand in
     * both calls -- a path here, a guest EA there -- so this is the same syscall
     * with the file resolved. */
    uint64_t saved_r4 = ctx->gpr[4], saved_r5 = ctx->gpr[5];
    ctx->gpr[4] = staged_ea;
    ctx->gpr[5] = 1;                                            /* DIRECT */
    int64_t rc = sys_spu_image_import_handler(ctx);
    ctx->gpr[4] = saved_r4;
    ctx->gpr[5] = saved_r5;

    /* sys_spu_image_open reports success/failure the same way, so let the
     * import result stand rather than overwriting it with a soft 0. */
    return rc;
}

/* ---------------------------------------------------------------------------
 * Process control
 *
 * sys_process_exit is how a guest ends its process when it is not linked
 * against the sysPrxForUser wrapper -- a bare-metal or PSL1GHT-style image
 * issues `sc` with r11 = 3 and nothing else. That number was never registered,
 * so it fell through to the catch-all stub in lv2_syscall(), which logs
 * "lv2_syscall 3 (stub)" and returns CELL_OK: the guest was told its own exit
 * succeeded and carried on executing past it. Route both process syscalls at
 * the same implementation the import path uses.
 * -----------------------------------------------------------------------*/
extern void sys_process_exit(int32_t exitcode);   /* libs/system/sysPrxForUser.c */
extern int32_t sys_process_getpid(void);

static int64_t sys_process_exit_handler(ppu_context* ctx)
{
    sys_process_exit((int32_t)ctx->gpr[3]);   /* does not return */
    return 0;
}

static int64_t sys_process_getpid_handler(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(uint32_t)sys_process_getpid();
    return (int64_t)(int32_t)ctx->gpr[3];
}

/* sys_process_get_sdk_version (25)
 *
 * r3 = pid, r4 = guest address to store the version at.
 *
 * The number is 25, not the 23 an ordering of the sys_process family would
 * suggest, and this runtime had no name and no handler for it -- so a caller
 * got CELL_ENOSYS and a version it never wrote.
 *
 * libsre asks at startup, through _cellSpursGetSdkVersion, and an SDK version
 * is how it picks between feature sets. Failing the call does not stop it: it
 * trips the assert at usertrace.c:123, prints the SDK internal assertion
 * banner and carries on with no version at all, which is a worse place to be
 * than either answer, because everything version-gated after it is decided
 * against nothing.
 *
 * g_ps3_sdk_version is the value a port sets from its title's PROC_PARAM
 * segment, which is where the number really comes from -- the loader reads
 * sys_process_param_t.sdk_version out of the ELF. The pid in r3 is ignored:
 * there is one process here.
 */
static int64_t sys_process_get_sdk_version_handler(ppu_context* ctx)
{
    uint32_t version_ea = (uint32_t)ctx->gpr[4];
    if (!version_ea) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EFAULT;
        return (int64_t)(int32_t)CELL_EFAULT;
    }
    const char* override = getenv("PS3_SDK_VERSION");
    uint32_t version = override && *override ? (uint32_t)strtoul(override, NULL, 0) : g_ps3_sdk_version;
    vm_write_be32(version_ea, version);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_process_is_spu_lock_line_reservation_address (14)
 *
 * r3 = effective address, r4 = access-right flags (SPU_THR 0x2, RAW_SPU 0x1).
 * Asks lv2 whether SPUs may place lock-line reservations -- GETLLAR/PUTLLC --
 * on a range. SPURS calls it while validating its management areas during
 * cellSpursInitialize, and treats a failure as a reason to abort init, so the
 * unimplemented-syscall handler's CELL_ENOSYS takes a title down a path that
 * ends in a null management pointer rather than anywhere that names this.
 *
 * The number has been in lv2_syscall_table.h since it was written and nothing
 * ever registered a handler for it.
 *
 * Contract from RPCS3's sys_process.cpp, mapped onto this runtime's guest
 * layout: the flags must be non-zero and contain only the two SPU bits, or
 * EINVAL; main memory, the sys_memory window and RSX local memory are
 * reservation-capable; PPU stacks and sys_vm regions are not, and answer
 * EPERM; anything outside those is EINVAL. */
static int64_t sys_process_is_spu_lock_line_reservation_address(ppu_context* ctx)
{
    uint32_t addr  = (uint32_t)ctx->gpr[3];
    uint64_t flags = ctx->gpr[4];
    int64_t  rc;

    if (!flags || (flags & ~0x3ull)) {
        rc = (int64_t)(int32_t)CELL_EINVAL;
    } else if (addr >= VM_MAIN_MEM_BASE && addr < VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE) {
        rc = 0;                                   /* main memory */
    } else if (addr >= 0x40000000u && addr < 0x50000000u) {
        rc = 0;                                   /* sys_memory window */
    } else if (addr >= 0xC0000000u && addr < 0xD0000000u) {
        rc = 0;                                   /* RSX local memory */
    } else if (addr >= 0xD0000000u && addr < 0xE0000000u) {
        rc = (int64_t)(int32_t)CELL_EPERM;        /* PPU stack area */
    } else if (addr >= SYS_VM_REGION_BASE && addr < SYS_VM_REGION_END) {
        rc = (int64_t)(int32_t)CELL_EPERM;        /* sys_vm memory */
    } else {
        rc = (int64_t)(int32_t)CELL_EINVAL;       /* unmapped */
    }

    ctx->gpr[3] = (uint64_t)rc;
    return rc;
}

/* sys_spu_thread_write_snr (sc-184): write an SPU Signal Notification Register.
 * In OR mode (spu_cfg bit 0/1) the value is ORed into the pending register;
 * in overwrite mode (default) it replaces it. The SPU reads via RdSigNotify1/2
 * (read-and-clear; channel count = 1 while a value is pending). */
static int64_t sys_spu_thread_write_snr_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t num = (uint32_t)ctx->gpr[4];
    uint32_t val = (uint32_t)ctx->gpr[5];
    if (num > 1) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return -1;
    }
    AcquireSRWLockShared(&s_live_ctx_lock);
    spu_context* sc = t->live_ctx ? (spu_context*)t->live_ctx : t->sctx;
    if (sc) {
        spu_channel* ch = &sc->ch_sig_notify[num];
        int or_mode = (t->spu_cfg >> num) & 1;
        if (or_mode) spu_channel_or(ch, val);
        else         spu_channel_overwrite(ch, val);      /* one word, replaced */
        extern void spu_ch_wake(spu_context*);
        spu_ch_wake(sc);
        { static _Atomic int n = 0; if (n < 12) { n++;
            fprintf(stderr, "[SPU] write_snr tid=0x%X snr%u <- 0x%08X (%s)\n",
                    tid, num + 1, val, or_mode ? "OR" : "overwrite");
            fflush(stderr); } }
    }
    ReleaseSRWLockShared(&s_live_ctx_lock);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_set_spu_cfg (sc-187): bits 0-1 = SNR1/SNR2 OR mode. */
static int64_t sys_spu_thread_set_spu_cfg_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint64_t val = (uint64_t)ctx->gpr[4];
    if (val & ~3ull) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return -1;
    }
    t->spu_cfg = (uint32_t)val;
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_get_spu_cfg (sc-188): read back the cfg word. */
static int64_t sys_spu_thread_get_spu_cfg_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005;
        return -1;
    }
    ctx->gpr[4] = (uint64_t)t->spu_cfg;
    ctx->gpr[3] = 0;
    return 0;
}

/* Catch-all stub for SPU syscalls we don't model individually yet. */
/* sys_spu_thread_{set,get}_spu_cfg -- the SPU's signal-notification config
 * word. Both were stubs, which means set() dropped the value and get() handed
 * back whatever the stub returns; a title that writes a config and reads it
 * back to confirm sees a mismatch. Virtua Fighter 5 calls both, once each,
 * exactly as its "SPU Delegate" group starts.
 *
 * There is nothing to configure on our side -- the lifted SPU code does not
 * consult it -- so this is storage, per thread, which is all the ABI promises
 * the caller. */
extern void vm_write64(uint64_t a, uint64_t v);



static int64_t sys_spu_thread_stub(ppu_context* ctx)
{
    (void)ctx;
    ctx->gpr[3] = 0;
    return 0;
}



/* sys_usbd_receive_event (540) -- a BLOCKING receive, not a poll.
 *
 * ps1_netemu starts a USB daemon thread (it gets that far now that _sys_malloc
 * works) whose whole body is: receive an event, dispatch it, repeat -- event 4
 * ends the thread, 3 is handled locally, 1 and 2 are forwarded with
 * sys_event_port_send. The unimplemented stub returned CELL_OK immediately with
 * the out-params untouched, so the guest read event type 0 and went straight
 * round again: 384,339 calls in a 45-second run, one thread burning a core flat
 * out and starving the SPUs that actually have work to do.
 *
 * On hardware and in RPCS3 this call SLEEPS until an event is queued. We have no
 * USB devices and no event source, so no event will ever arrive.
 * ponytail: sleep-and-return rather than a real wait queue -- it parks the thread
 * at ~50 Hz instead of blocking forever, so nothing can wedge on shutdown, and
 * the guest simply loops. Give it a real queue if a title ever needs USB events
 * (a pad through the USB stack rather than cellPad, say).
 */
static int64_t sys_usbd_receive_event_handler(ppu_context* ctx)
{
    uint32_t a1 = (uint32_t)ctx->gpr[4];
    uint32_t a2 = (uint32_t)ctx->gpr[5];
    uint32_t a3 = (uint32_t)ctx->gpr[6];
    /* Report "no event" explicitly; the stub left the guest reading its own
     * stack, which only happened to be zero. */
    if (a1) { vm_write_be32(a1, 0); vm_write_be32(a1 + 4, 0); }
    if (a2) { vm_write_be32(a2, 0); vm_write_be32(a2 + 4, 0); }
    if (a3) { vm_write_be32(a3, 0); vm_write_be32(a3 + 4, 0); }
#ifdef _WIN32
    Sleep(20);
#else
    { struct timespec ts = {0, 20*1000*1000}; nanosleep(&ts, 0); }
#endif
    return CELL_OK;
}

/* Numbers retail lv2 does not implement (DEX/debug-only or unassigned; RPCS3's
 * uns_func entries): the kernel answers ENOSYS. liblv2 probes at least one of
 * them (462) at process start and takes a different path on success, so
 * answering these CELL_OK like the generic unimplemented fallback misleads it. */
static const uint16_t s_lv2_unused_syscalls[] = {
    6, 15, 20, 32, 42, 59, 79, 162, 164, 168, 183, 189, 195, 217, 218, 219, 241,
    255, 261, 270, 280, 290, 316, 347, 366, 371, 399, 416, 420, 430, 440, 459,
    462, 469, 477, 491, 515, 526, 576, 629, 632, 660, 697, 698, 727, 730, 740,
    750, 760, 770, 780, 790, 848, 854, 886, 898, 958, 973, 990, 999, 1008, 1020,
};
static int64_t sys_unused_enosys(ppu_context* ctx) { (void)ctx; return (int32_t)CELL_ENOSYS; }

void lv2_prx_register_syscalls(lv2_syscall_table* tbl);   /* lv2_prx.c */
void sys_lwsync_register(lv2_syscall_table* tbl);         /* sys_lwsync.c */

void lv2_register_all_syscalls(lv2_syscall_table* tbl)
{
    /* Initialize the table with unimplemented stubs first */
    lv2_syscall_table_init(tbl);
    for (size_t i = 0; i < sizeof s_lv2_unused_syscalls / sizeof s_lv2_unused_syscalls[0]; i++)
        lv2_syscall_register(tbl, s_lv2_unused_syscalls[i], sys_unused_enosys);
    lv2_prx_register_syscalls(tbl);
    sys_lwsync_register(tbl);

    /* Process control */
    lv2_syscall_register(tbl, SYS_PROCESS_GETPID, sys_process_getpid_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_EXIT,   sys_process_exit_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_GET_SDK_VERSION, sys_process_get_sdk_version_handler);
    lv2_syscall_register(tbl, SYS_PROCESS_IS_SPU_LOCK_LINE_RESERVATION_ADDRESS,
                         sys_process_is_spu_lock_line_reservation_address);

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

    /* RSX (libs/video/sys_rsx.c). Only a guest that talks to RSX through the
     * kernel needs these -- a title that imports cellGcmSys never issues one.
     * PS3 firmware modules link libgcm statically and go straight here. */
    sys_rsx_init(tbl);

    /* Raw SPUs (runtime/spu/spu_raw.c). Registered AFTER the SPU-thread block
     * below would be wrong -- 150..154 and 160/161 are raw-SPU numbers and the
     * generic SPU stubs must not claim them -- so keep this ahead of it and let
     * the loud stub handler cover anything neither owns. */
    sys_raw_spu_init(tbl);

    /* TTY (debug console I/O — used by CRT startup) */
    lv2_syscall_register(tbl, SYS_TTY_READ,  sys_tty_read);
    lv2_syscall_register(tbl, SYS_TTY_WRITE, sys_tty_write);
    /* Some SDK-era CRTs (Tokyo Jungle, Sonic/Gunstar hubs, 4 Elements HD) issue
     * sys_tty_write under the alternate number 988 (0x3DC) instead of 403; an
     * unimplemented return derails the CRT init table-walk into abort(). Alias it. */
    lv2_syscall_register(tbl, 988, sys_tty_write);

    /* sys_ss_get_open_psid (console PSN/NP identity) — LBP 1.30 reads it during
     * boot; the unimplemented stub left the out-param as garbage. */
    lv2_syscall_register(tbl, 872, sys_ss_get_open_psid_handler);

    /* sys_process_get_sdk_version (25). Reported as a stub for a long time and
     * returning CELL_OK with the out-param untouched, which reads as SDK 0.
     * That is not harmless: libgcm sizes the RSX local-memory heap off it with
     * a compatibility ladder (>=2.20 -> 249 MB, >=2.00 -> 242, >=1.90 -> 234,
     * >=1.80 -> 232, else 224), and ps1_netemu's cellGcmInit rejects a zero
     * outright -- which is what "[GPU] cellGcmInit failed" was: a process
     * syscall, not anything to do with RSX. Report a modern SDK, since the HLE
     * this runtime implements is the modern one. PS3_SDK_VERSION overrides. */
    lv2_syscall_register(tbl, 25, sys_process_get_sdk_version_handler);

    /* USB daemon event receive -- blocking on hardware; see the handler. */
    lv2_syscall_register(tbl, 540, sys_usbd_receive_event_handler);

    /* SPU syscalls — we don't execute SPU code but the PPU-side wrappers
     * need consistent IDs and out-params. See the stateful group tracker
     * above for contract notes. */
    lv2_syscall_register(tbl, 169,                            sys_spu_thread_stub); /* deprecated */
    lv2_syscall_register(tbl, SYS_SPU_INITIALIZE,             sys_spu_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_OPEN,             sys_spu_image_open_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_IMPORT,           sys_spu_image_import_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_CLOSE,            sys_spu_image_close_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_GET_SEGMENTS,     sys_spu_image_get_segments_handler);
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
    { extern int (*g_spu_user_event_hook)(spu_context*, uint32_t);
      g_spu_user_event_hook = spu_deliver_user_event;
      { extern int (*g_spu_lv2_stop_hook)(spu_context*); g_spu_lv2_stop_hook = spu_lv2_stop_service; } }
    lv2_syscall_register(tbl, SYS_SPU_THREAD_DISCONNECT_EVENT,sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT, sys_spu_thread_group_connect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT, sys_spu_thread_group_disconnect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_LS,        sys_spu_thread_write_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_READ_LS,         sys_spu_thread_read_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SNR,       sys_spu_thread_write_snr_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_SET_SPU_CFG,    sys_spu_thread_set_spu_cfg_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GET_SPU_CFG,    sys_spu_thread_get_spu_cfg_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SPU_MB,  sys_spu_thread_write_spu_mb_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_BIND_QUEUE,      sys_spu_thread_bind_queue_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_UNBIND_QUEUE,    sys_spu_thread_unbind_queue_handler);
    lv2_syscall_register(tbl, 252, sys_spu_thread_group_disconnect_event_all_threads_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS, sys_spu_thread_group_connect_event_all_threads_handler);
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

/* Firmware imports that are ALSO lv2 syscalls.
 *
 * A title can reach these two ways: issue the raw `sc` (-> the syscall table
 * above) or call the sysPrxForUser userland wrapper by NID (-> ps3_hle_call).
 * LBP does the latter, and an unregistered NID falls to the unresolved-NID
 * stub, which returns CELL_OK WITHOUT touching the caller's out-params -- so
 * the caller reads its own uninitialised stack as the result. For
 * sys_spu_image_import that means a garbage sys_spu_image {segs, nsegs}, and
 * the caller (LBP func_00483498) then walks the bogus segment array until it
 * runs off the end of memory: on hardware that segfaults immediately, but our
 * demand-committed flat VM answers every stray read with a zero page, so it
 * silently swept ~3 GB of address space and hung the boot.
 *
 * Bridge them onto the NID path so both entries hit the same implementation.
 * The handlers already take the ppu_context and set gpr[3] themselves. */
extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

#define LV2_HLE_BRIDGE(fn_name, handler)                                       static void fn_name(ppu_context* ctx) { (void)handler(ctx); }

LV2_HLE_BRIDGE(hle_sys_spu_image_import, sys_spu_image_import_handler)
LV2_HLE_BRIDGE(hle_sys_spu_image_open,   sys_spu_image_open_handler)

void lv2_init_syscalls(void)
{
    lv2_register_all_syscalls(&g_lv2_syscalls);

    ps3_hle_register_ctx(ps3_compute_nid("sys_spu_image_import"),
                         "sys_spu_image_import", hle_sys_spu_image_import);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spu_image_open"),
                         "sys_spu_image_open",   hle_sys_spu_image_open);
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
    /* YDKJ diag: full event-syscall trace (#128..141) during SPURS init to find
     * why libsre asserts ESRCH in event_helper.c. Snapshot args BEFORE handler. */
    uint32_t _a3 = (uint32_t)ctx->gpr[3], _a4 = (uint32_t)ctx->gpr[4], _a5 = (uint32_t)ctx->gpr[5];
    uint32_t _a6 = (uint32_t)ctx->gpr[6], _a7 = (uint32_t)ctx->gpr[7], _a8 = (uint32_t)ctx->gpr[8],
             _a9 = (uint32_t)ctx->gpr[9];
    ctx->gpr[3] = (uint64_t)h(ctx);
    /* LV2_ERRDBG=1: every syscall that returns non-OK, deduped by (number,
     * result). A guest that asserts on a result once per frame is easier to
     * find from this side than by reading its lifted code. */
    { static _Atomic int _ed = -1; if (_ed < 0) _ed = getenv("LV2_ERRDBG") ? 1 : 0;
      if (_ed && (int32_t)ctx->gpr[3] != 0) {
          static uint64_t seen[64]; static int ns = 0;
          uint64_t k = ((uint64_t)num << 32) | (uint32_t)ctx->gpr[3];
          int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == k) f = 1;
          if (!f && ns < 64) { seen[ns++] = k;
              fprintf(stderr, "[lv2err] syscall %u(r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X"
                              " r7=0x%08X r8=0x%08X r9=0x%08X) -> 0x%08X lr=0x%08X%c",
                      num, _a3, _a4, _a5, _a6, _a7, _a8, _a9, (uint32_t)ctx->gpr[3],
                      (uint32_t)ctx->lr, 10); } } }
    return 1;
}
