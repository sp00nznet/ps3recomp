/*
 * ps3recomp - the SPURS kernel on the SPUs, and libsre's two PPU helpers
 *
 * A SPURS instance owns an SPU thread group: every SPU runs the SPURS kernel,
 * which repeatedly selects a workload from the shared instance and enters its
 * policy module at LS 0xA00. The kernel's own "system service" workload runs
 * whenever an SPU has a message for it or nothing else to do: it re-reads the
 * workload table (wklStatus, runnable mask, per-SPU priorities), completes
 * shutdowns, and idles the SPU. On the PPU side libsre keeps a handler thread
 * (starts / restarts the group: exitIfNoWork) and an event helper thread
 * (shutdown completion: hook, waiter's semaphore).
 *
 * This file models those parts as they behave, not just what one title needs:
 * one host thread per SPU with its own local store (a module image is loaded
 * only when the SPU switches to a workload at another address, as the kernel
 * does), the kernel's selection rule and contention bookkeeping, the
 * system-service handshakes in the instance, and the two PPU helpers. The
 * behaviour is checked against firmware libsre under RPCS3 by
 * tests/conformance/spurs (t_workload, t_kernel).
 *
 * Instance updates are made with byte/word atomics: the title's inlined SDK
 * code updates the same words with lwarx/stwcx., which this runtime implements
 * as compare-and-swap on the reserved value, so either side's change makes the
 * other's store-conditional retry. The SPUs of one instance serialise their
 * kernel-side read-modify-writes with a host lock, standing in for the
 * GETLLAR/PUTLLC loops the real kernel uses on the instance's lines.
 */
#include "spurs_instance.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "spu_workload.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KMAX_INST 4
#define KMAX_SPU  8

typedef struct KInst KInst;

typedef struct {
    KInst*       in;
    uint32_t     num;            /* spuNum */
    spu_context* ctx;            /* this SPU: registers + local store */
    uint32_t     cur_wid;        /* wklCurrentId (SPURS_SYS_SERVICE_WID: the system service) */
    uint32_t     cur_uid;        /* wklCurrentUniqueId */
    uint64_t     cur_addr;       /* wklCurrentAddr: the module image now in LS */
    uint8_t      loc_cont[SPURS_MAX_WKL], loc_pend[SPURS_MAX_WKL];
    uint8_t      prio[SPURS_MAX_WKL], uid[SPURS_MAX_WKL];
    uint16_t     runnable1;
    uint8_t      idling, sys_init;
    spu_lifted_entry_fn fn;      /* lifted entry of the loaded image, NULL = interpret */
    int          image_id;
} KSpu;

struct KInst {
    uint32_t           ea;               /* 0 = free slot */
    uint32_t           nspus;
    SRWLOCK            lock;             /* kernel-side instance RMWs */
    CONDITION_VARIABLE idle_cv;          /* idling SPUs */
    KSpu               spu[KMAX_SPU];
    HANDLE             spu_th[KMAX_SPU];
    /* handler thread (group life cycle) */
    HANDLE             handler_th;
    SRWLOCK            hlock;
    CONDITION_VARIABLE hcv;
    volatile int       exiting;
    /* event helper thread (shutdown completion) */
    HANDLE             event_th;
    volatile uint32_t  notify;           /* wid bits (31-wid) awaiting the helper */
    uint32_t           sema[SPURS_MAX_WKL];
    CONDITION_VARIABLE scv;
};

static KInst s_kinst[KMAX_INST];
static SRWLOCK s_table = SRWLOCK_INIT;

static uint8_t* B(uint32_t ea) { return vm_base + ea; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static void bor(uint8_t* p, uint8_t m)  { __atomic_fetch_or(p, m, __ATOMIC_SEQ_CST); }
static void band(uint8_t* p, uint8_t m) { __atomic_fetch_and(p, m, __ATOMIC_SEQ_CST); }

static KInst* kinst_find(uint32_t ea)
{
    for (int i = 0; i < KMAX_INST; i++)
        if (s_kinst[i].ea == ea) return &s_kinst[i];
    return NULL;
}

/* ---- selectWorkload -------------------------------------------------------------- */

/* The kernel's selection (SPURS1). is_poll: called by a running module
 * (cellSpursModulePollStatus) rather than by the kernel between modules --
 * then nothing is committed except pending contention for a switch.
 * Returns {wid << 32 | pollStatus}. */
static uint64_t kselect(KSpu* v, int is_poll)
{
    KInst* in = v->in;
    uint8_t* S = B(in->ea);
    const uint8_t bit = (uint8_t)(1u << v->num);
    uint8_t cont[SPURS_MAX_WKL], pend[SPURS_MAX_WKL];
    uint32_t sel = SPURS_SYS_SERVICE_WID, status = 0;

    AcquireSRWLockExclusive(&in->lock);
    for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
        cont[i] = (uint8_t)(S[SPURS_WKL_CURCONT + i] - v->loc_cont[i]);
        pend[i] = 0;
        if (is_poll) {
            pend[i] = (uint8_t)(S[SPURS_WKL_PENDCONT + i] - v->loc_pend[i]);
            if (i != v->cur_wid) cont[i] = (uint8_t)(cont[i] + pend[i]);
        }
    }

    if (__atomic_load_n(S + SPURS_SYSSRV_MESSAGE, __ATOMIC_SEQ_CST) & bit) {
        /* the system service comes first */
        v->idling = 0;
        if (!is_poll || v->cur_wid == SPURS_SYS_SERVICE_WID)
            band(S + SPURS_SYSSRV_MESSAGE, (uint8_t)~bit);
    } else {
        const uint16_t sig = (uint16_t)(S[SPURS_WKL_SIGNAL1] << 8 | S[SPURS_WKL_SIGNAL1 + 1]);
        const uint32_t flag = be32(S + SPURS_WKL_FLAG + 0x0C);
        const uint8_t rcv = S[SPURS_WKL_FLAG_RCV];
        uint32_t max_weight = 0;
        for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
            const int runnable = (v->runnable1 & (0x8000u >> i)) != 0;
            const int wsig = (sig & (0x8000u >> i)) != 0;
            const int wflag = flag == 0 && rcv == i;
            const uint32_t ready = S[SPURS_WKL_READY1 + i] > 8 ? 8 : S[SPURS_WKL_READY1 + i];
            const uint32_t idle = S[SPURS_WKL_IDLE2 + i] > 8 ? 8 : S[SPURS_WKL_IDLE2 + i];
            if (!runnable || !v->prio[i] || S[SPURS_WKL_MAXCONT + i] <= cont[i]) continue;
            if (!(wflag || wsig || (ready && ready + idle > cont[i]))) continue;
            /* weight, most significant first: wants an SPU, priority on this
             * SPU, is the current workload, below its minimum contention,
             * fewer SPUs already, same module image loaded; ties to the
             * lowest wid */
            uint32_t w = (wflag || wsig || ready > cont[i]) ? 0x8000u : 0;
            w |= (uint32_t)(v->prio[i] & 0x7F) << 8;
            w |= i == v->cur_wid ? 0x80u : 0;
            w |= (cont[i] > 0 && S[SPURS_WKL_MINCONT + i] > cont[i]) ? 0x40u : 0;
            w |= (uint32_t)((8 - cont[i]) & 0x0F) << 2;
            w |= v->uid[i] == v->cur_uid ? 0x02u : 0;
            w |= 1;
            if (w > max_weight) {
                max_weight = w;
                sel = i;
                status = (ready > cont[i] ? SPURS_POLL_READYCOUNT : 0) |
                         (wsig ? SPURS_POLL_SIGNAL : 0) | (wflag ? SPURS_POLL_FLAG : 0);
            }
        }
        v->idling = sel == SPURS_SYS_SERVICE_WID;
        if (!is_poll || sel == v->cur_wid) {
            if (sel < SPURS_MAX_WKL) {
                /* the signal is taken by this SPU; the flag is re-armed */
                band(S + SPURS_WKL_SIGNAL1 + (sel >> 3), (uint8_t)~(0x80u >> (sel & 7)));
                if (sel == rcv)
                    __atomic_store_n((uint32_t*)(S + SPURS_WKL_FLAG + 0x0C), 0xFFFFFFFFu,
                                     __ATOMIC_SEQ_CST);
            }
        }
    }

    if (!is_poll) {
        if (sel < SPURS_MAX_WKL) cont[sel]++;
        for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
            S[SPURS_WKL_CURCONT + i] = cont[i];
            S[SPURS_WKL_PENDCONT + i] = (uint8_t)(S[SPURS_WKL_PENDCONT + i] - v->loc_pend[i]);
            v->loc_cont[i] = 0;
            v->loc_pend[i] = 0;
        }
        if (sel < SPURS_MAX_WKL) v->loc_cont[sel] = 1;
        v->cur_wid = sel;
    } else if (sel != v->cur_wid) {
        if (sel < SPURS_MAX_WKL) pend[sel]++;
        for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
            S[SPURS_WKL_PENDCONT + i] = pend[i];
            v->loc_pend[i] = 0;
        }
        if (sel < SPURS_MAX_WKL) v->loc_pend[sel] = 1;
    } else {
        for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
            S[SPURS_WKL_PENDCONT + i] = (uint8_t)(S[SPURS_WKL_PENDCONT + i] - v->loc_pend[i]);
            v->loc_pend[i] = 0;
        }
    }
    /* the kernel keeps a copy of the scheduling line at LS 0x100 */
    memcpy(v->ctx->ls + 0x100, S, 0x80);
    ReleaseSRWLockExclusive(&in->lock);
    return (uint64_t)sel << 32 | status;
}

/* selectWorkload called by a running module (lifted or interpreted). */
static uint64_t kselect_from_module(spu_context* ctx, uint32_t is_poll)
{
    KSpu* v = (KSpu*)ctx->spurs_vspu;
    return kselect(v, is_poll != 0);
}

/* ---- the kernel context in local store ------------------------------------------ */

static void ls_context(KSpu* v)
{
    uint8_t* ls = v->ctx->ls;
    put64(ls + 0x1C0, v->in->ea);                   /* spurs */
    put32(ls + 0x1C8, v->num);                      /* spuNum */
    put32(ls + 0x1CC, 31);                          /* dmaTagId */
    put64(ls + 0x1D0, v->cur_addr);                 /* wklCurrentAddr */
    put32(ls + 0x1D8, v->cur_uid);                  /* wklCurrentUniqueId */
    put32(ls + 0x1DC, v->cur_wid);                  /* wklCurrentId */
    put32(ls + 0x1E0, SPURS_PM_EXIT_TO_KERNEL_LS);  /* exitToKernelAddr */
    put32(ls + 0x1E4, SPURS_PM_SELECT_WORKLOAD_LS); /* selectWorkloadAddr */
    ls[0x1E8] = 0; ls[0x1E9] = 0;                   /* moduleId */
    ls[0x1EA] = v->sys_init;                        /* sysSrvInitialised */
    ls[0x1EB] = v->idling;                          /* spuIdling */
    ls[0x1EC] = (uint8_t)(v->runnable1 >> 8);       /* wklRunnable1 */
    ls[0x1ED] = (uint8_t)v->runnable1;
    ls[0x1EE] = 0; ls[0x1EF] = 0;                   /* wklRunnable2 */
    memcpy(ls + 0x220, v->uid, SPURS_MAX_WKL);      /* wklUniqueId[] */
}

/* ---- shutdown completion: to the event helper ----------------------------------- */

static void kevent_notify(KInst* in, uint32_t bits)
{
    AcquireSRWLockExclusive(&in->hlock);
    in->notify |= bits;
    WakeAllConditionVariable(&in->scv);
    ReleaseSRWLockExclusive(&in->hlock);
}

/* ---- the system service ---------------------------------------------------------- */

/* The update-workload message: this SPU's priorities and uniqueIds, its bit in
 * every workload's status, its runnable mask; a shutting-down workload this
 * SPU was the last to hold becomes removable. */
static void kactivate(KSpu* v)
{
    KInst* in = v->in;
    uint8_t* S = B(in->ea);
    const uint8_t bit = (uint8_t)(1u << v->num);
    for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
        const uint8_t* info = S + SPURS_WKL_INFO1 + i * SPURS_WKL_INFO_SZ;
        const uint8_t p = info[0x18 + v->num];
        v->prio[i] = p ? (uint8_t)(0x10 - p) : 0;
        v->uid[i] = info[0x14];
    }
    uint32_t shutdown = 0, notify = 0;
    uint16_t runnable = 0;
    AcquireSRWLockExclusive(&in->lock);
    for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
        const uint8_t old = S[SPURS_WKL_STATUS1 + i];
        const uint8_t state = __atomic_load_n(S + SPURS_WKL_STATE1 + i, __ATOMIC_SEQ_CST);
        if (state == WKL_STATE_RUNNABLE) {
            bor(S + SPURS_WKL_STATUS1 + i, bit);
            runnable |= (uint16_t)(0x8000u >> i);
        } else {
            band(S + SPURS_WKL_STATUS1 + i, (uint8_t)~bit);
        }
        if (state == WKL_STATE_SHUTTING_DOWN && (old & bit) && S[SPURS_WKL_STATUS1 + i] == 0) {
            uint8_t st = WKL_STATE_SHUTTING_DOWN;
            if (__atomic_compare_exchange_n(S + SPURS_WKL_STATE1 + i, &st, WKL_STATE_REMOVABLE, 0,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                shutdown |= 0x80000000u >> i;
        }
    }
    v->runnable1 = runnable;
    for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
        if (!(shutdown & (0x80000000u >> i))) continue;
        bor(S + SPURS_WKL_EVENT1 + i, 0x01);
        if (S[SPURS_WKL_EVENT1 + i] & 0x12)          /* a hook, or a waiter */
            notify |= 0x80000000u >> i;
    }
    ReleaseSRWLockExclusive(&in->lock);
    if (notify) kevent_notify(in, notify);
}

/* Returns 1 if this SPU must leave (terminate request). */
static int kprocess_requests(KSpu* v)
{
    uint8_t* S = B(v->in->ea);
    const uint8_t bit = (uint8_t)(1u << v->num);
    int terminate = 0, update = 0;
    AcquireSRWLockExclusive(&v->in->lock);
    if (S[SPURS_SYSSRV_TERMINATE] & bit) {
        band(S + SPURS_SYSSRV_ON_SPU, (uint8_t)~bit);
        terminate = 1;
    }
    if (S[SPURS_SYSSRV_MSG] & bit) {
        band(S + SPURS_SYSSRV_MSG, (uint8_t)~bit);
        update = 1;
    }
    ReleaseSRWLockExclusive(&v->in->lock);
    if (update) kactivate(v);
    return terminate;
}

static int kready_workload(KSpu* v)
{
    const uint8_t* S = B(v->in->ea);
    const uint16_t sig = (uint16_t)(S[SPURS_WKL_SIGNAL1] << 8 | S[SPURS_WKL_SIGNAL1 + 1]);
    const uint32_t flag = be32(S + SPURS_WKL_FLAG + 0x0C);
    for (uint32_t i = 0; i < SPURS_MAX_WKL; i++) {
        const uint32_t ready = S[SPURS_WKL_READY1 + i] > 8 ? 8 : S[SPURS_WKL_READY1 + i];
        const uint32_t idle = S[SPURS_WKL_IDLE2 + i] > 8 ? 8 : S[SPURS_WKL_IDLE2 + i];
        const uint8_t c = S[SPURS_WKL_CURCONT + i];
        if (!(v->runnable1 & (0x8000u >> i)) || !v->prio[i] || S[SPURS_WKL_MAXCONT + i] <= c)
            continue;
        if ((flag == 0 && S[SPURS_WKL_FLAG_RCV] == i) || (sig & (0x8000u >> i)) ||
            (ready && ready + idle > c))
            return 1;
    }
    return 0;
}

/* Idle until there is something to do. Returns 1 if the group must exit
 * (every SPU idle and the instance was created exitIfNoWork). */
static int kidle(KSpu* v)
{
    KInst* in = v->in;
    uint8_t* S = B(in->ea);
    const uint8_t bit = (uint8_t)(1u << v->num);
    for (;;) {
        AcquireSRWLockExclusive(&in->lock);
        const uint8_t idling = S[SPURS_SPU_IDLING];
        const int all_idle = (uint32_t)__builtin_popcount(idling) == S[SPURS_NSPUS];
        const int should_exit = all_idle && (S[SPURS_FLAGS1] & SPURS_SF1_EXIT_IF_NO_WORK);
        const int found = (S[SPURS_SYSSRV_MESSAGE] & bit) || kready_workload(v);
        const int was_idle = (idling & bit) != 0;
        if (found && !should_exit) band(S + SPURS_SPU_IDLING, (uint8_t)~bit);
        else                       bor(S + SPURS_SPU_IDLING, bit);
        if (was_idle && !should_exit && !found) {
            /* The real SPU waits for its reservation on the instance to be
             * lost; here: woken by the PPU library, or after 1 ms to see
             * stores the title makes itself. */
            SleepConditionVariableSRW(&in->idle_cv, &in->lock, 1, 0);
            ReleaseSRWLockExclusive(&in->lock);
            continue;
        }
        ReleaseSRWLockExclusive(&in->lock);
        return should_exit;
    }
}

enum { SYS_TO_KERNEL = 0, SYS_TERMINATE = 1, SYS_EXIT_GROUP = 2 };

static int ksys_service(KSpu* v)
{
    uint8_t* S = B(v->in->ea);
    if (!v->sys_init) {
        v->sys_init = 1;
        bor(S + SPURS_SYSSRV_ON_SPU, (uint8_t)(1u << v->num));
        bor(S + SPURS_SYSSRV_TRACE_INIT, (uint8_t)(1u << v->num));
    }
    for (;;) {
        if (kprocess_requests(v)) return SYS_TERMINATE;
        for (;;) {
            if ((uint32_t)(kselect(v, 1) >> 32) != SPURS_SYS_SERVICE_WID)
                return SYS_TO_KERNEL;            /* a workload wants this SPU */
            if (!v->idling) break;               /* more messages */
            if (kidle(v)) return SYS_EXIT_GROUP;
        }
    }
}

/* ---- dispatching a workload ------------------------------------------------------- */

static void kdispatch(KSpu* v, uint32_t wid, uint32_t status)
{
    const uint8_t* info = B(v->in->ea) + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ;
    const uint64_t addr = be64(info), arg = be64(info + 8);
    const uint32_t size = be32(info + 0x10);
    if (addr != v->cur_addr) {
        if (!addr || size > SPU_LS_SIZE - 0xA00) {
            fprintf(stderr, "[spurs-kern] wid %u: module 0x%llX size 0x%X cannot be loaded\n",
                    wid, (unsigned long long)addr, size);
            return;
        }
        memcpy(v->ctx->ls + 0xA00, B((uint32_t)addr), size);
        const uint64_t fp = spu_workload_fingerprint(v->ctx->ls + 0xA00, size);
        v->image_id = 0;
        v->fn = spu_workload_find_img(fp, &v->image_id);
        if (!v->fn) {
            static uint64_t s_told[16]; static int s_n;
            int told = 0;
            for (int k = 0; k < s_n; k++) told |= s_told[k] == fp;
            if (!told && s_n < 16) {
                s_told[s_n++] = fp;
                fprintf(stderr, "[spurs-kern] module at 0x%llX (fp=0x%016llX, %u bytes) not lifted: "
                                "interpreting\n", (unsigned long long)addr, (unsigned long long)fp, size);
            }
        }
        v->cur_addr = addr;
        v->cur_uid = info[0x14];
    }
    ls_context(v);
    spu_pm_enter(v->ctx, v->fn, v->image_id, arg, addr, status);
}

/* ---- one SPU ---------------------------------------------------------------------- */

/* An SPU leaving a group that exits for lack of work: it no longer holds any
 * workload or runs the system service, and the update-workload message is
 * re-armed so that it reads the workload table again when restarted. */
static void kspu_leave(KSpu* v)
{
    uint8_t* S = B(v->in->ea);
    const uint8_t bit = (uint8_t)(1u << v->num);
    AcquireSRWLockExclusive(&v->in->lock);
    for (uint32_t i = 0; i < SPURS_MAX_WKL; i++)
        band(S + SPURS_WKL_STATUS1 + i, (uint8_t)~bit);
    band(S + SPURS_SPU_IDLING, (uint8_t)~bit);
    band(S + SPURS_SYSSRV_ON_SPU, (uint8_t)~bit);
    band(S + SPURS_SYSSRV_TRACE_INIT, (uint8_t)~bit);
    bor(S + SPURS_SYSSRV_MSG, bit);
    ReleaseSRWLockExclusive(&v->in->lock);
}

static DWORD WINAPI kspu_main(LPVOID p)
{
    KSpu* v = (KSpu*)p;
    for (;;) {
        /* the kernel starts in, and returns to, the system service */
        const int r = ksys_service(v);
        if (r == SYS_EXIT_GROUP) kspu_leave(v);
        if (r != SYS_TO_KERNEL) break;
        for (;;) {
            const uint64_t sel = kselect(v, 0);
            const uint32_t wid = (uint32_t)(sel >> 32);
            if (wid == SPURS_SYS_SERVICE_WID) break;
            kdispatch(v, wid, (uint32_t)sel);
        }
    }
    return 0;
}

static void kgroup_start(KInst* in)
{
    for (uint32_t n = 0; n < in->nspus; n++) {
        KSpu* v = &in->spu[n];
        if (!v->ctx) {
            v->ctx = (spu_context*)malloc(sizeof(spu_context));
            if (!v->ctx) continue;
        }
        /* a (re)started SPU thread loads the kernel afresh */
        memset(v->ctx, 0, sizeof(*v->ctx));
        spu_context_init(v->ctx, 0);
        v->ctx->spurs_vspu = v;
        v->in = in;
        v->num = n;
        v->cur_wid = SPURS_SYS_SERVICE_WID;
        v->cur_uid = 0x20;
        v->cur_addr = ~0ull;
        memset(v->loc_cont, 0, sizeof v->loc_cont);
        memset(v->loc_pend, 0, sizeof v->loc_pend);
        memset(v->prio, 0, sizeof v->prio);
        memset(v->uid, 0, sizeof v->uid);
        v->runnable1 = 0;
        v->idling = 0;
        v->sys_init = 0;
        v->fn = NULL;
        in->spu_th[n] = CreateThread(NULL, 1u << 20, kspu_main, v, 0, NULL);
    }
}

static void kgroup_join(KInst* in)
{
    for (uint32_t n = 0; n < in->nspus; n++) {
        if (!in->spu_th[n]) continue;
        WaitForSingleObject(in->spu_th[n], INFINITE);
        CloseHandle(in->spu_th[n]);
        in->spu_th[n] = NULL;
    }
}

/* ---- the PPU handler thread: the group's life cycle -------------------------------- */

enum { H_DIRTY = 0xD64, H_WAITING = 0xD65, H_EXITING = 0xD66 };

/* exitIfNoWork: wait until a workload is ready before (re)starting the group.
 * Re-checked only when WakeUp marks the handler dirty, as libsre does. */
static int khandler_wait_ready(KInst* in)
{
    uint8_t* S = B(in->ea);
    AcquireSRWLockExclusive(&in->hlock);
    for (;;) {
        if (in->exiting) { ReleaseSRWLockExclusive(&in->hlock); return 1; }
        S[H_DIRTY] = 0;
        const uint16_t sig = (uint16_t)(S[SPURS_WKL_SIGNAL1] << 8 | S[SPURS_WKL_SIGNAL1 + 1]);
        const uint32_t flag = be32(S + SPURS_WKL_FLAG + 0x0C);
        int found = 0;
        for (uint32_t i = 0; i < SPURS_MAX_WKL && !found; i++) {
            const uint8_t* info = S + SPURS_WKL_INFO1 + i * SPURS_WKL_INFO_SZ;
            if (S[SPURS_WKL_STATE1 + i] != WKL_STATE_RUNNABLE || !be64(info + 0x18) ||
                !(S[SPURS_WKL_MAXCONT + i] & 0x0F))
                continue;
            found = S[SPURS_WKL_READY1 + i] || (sig & (0x8000u >> i)) ||
                    (flag == 0 && S[SPURS_WKL_FLAG_RCV] == i);
        }
        if (found) break;
        S[H_WAITING] = 1;
        if (!S[H_DIRTY])
            SleepConditionVariableSRW(&in->hcv, &in->hlock, INFINITE, 0);
        S[H_WAITING] = 0;
    }
    ReleaseSRWLockExclusive(&in->hlock);
    return 0;
}

static DWORD WINAPI khandler_main(LPVOID p)
{
    KInst* in = (KInst*)p;
    for (;;) {
        const int exit_if_no_work = (B(in->ea)[SPURS_FLAGS1] & SPURS_SF1_EXIT_IF_NO_WORK) != 0;
        if (exit_if_no_work && khandler_wait_ready(in)) break;
        if (in->exiting) break;
        kgroup_start(in);
        kgroup_join(in);
        if (!exit_if_no_work || in->exiting) break;
    }
    return 0;
}

/* ---- the PPU event helper thread: shutdown completion ------------------------------ */

static DWORD WINAPI kevent_main(LPVOID p)
{
    KInst* in = (KInst*)p;
    const uint32_t ea = in->ea;
    uint8_t* S = B(ea);
    for (;;) {
        AcquireSRWLockExclusive(&in->hlock);
        while (!in->notify && !in->exiting)
            SleepConditionVariableSRW(&in->scv, &in->hlock, INFINITE, 0);
        const uint32_t bits = in->notify;
        in->notify = 0;
        const int exiting = in->exiting;
        ReleaseSRWLockExclusive(&in->hlock);
        for (uint32_t wid = 0; wid < SPURS_MAX_WKL; wid++) {
            if (!(bits & (0x80000000u >> wid))) continue;
            const uint8_t* f1 = S + SPURS_WKL_F1 + SPURS_WKL_F1_SZ * wid;
            const uint32_t hook = (uint32_t)be64(f1 + 0x30);
            if (hook) {
                ps3_invoke_guest(hook, ea, wid, be64(f1 + 0x38), 0, 0, 0, 0, 0);
                bor(S + SPURS_WKL_EVENT1 + wid, 0x20);
            }
            if (!hook || (S[SPURS_WKL_EVENT1 + wid] & 0x10)) {
                AcquireSRWLockExclusive(&in->hlock);
                in->sema[wid]++;
                WakeAllConditionVariable(&in->scv);
                ReleaseSRWLockExclusive(&in->hlock);
            }
        }
        if (exiting && !bits) break;
    }
    return 0;
}

/* ---- interface ------------------------------------------------------------------- */

void spurs_kernel_start(uint32_t ea, uint32_t nspus)
{
    g_spurs_kernel_select = kselect_from_module;
    AcquireSRWLockExclusive(&s_table);
    KInst* in = kinst_find(ea);
    if (!in) in = kinst_find(0);
    if (!in) {
        ReleaseSRWLockExclusive(&s_table);
        fprintf(stderr, "[spurs-kern] more than %d SPURS instances\n", KMAX_INST);
        return;
    }
    KSpu spus[KMAX_SPU];
    memcpy(spus, in->spu, sizeof spus);            /* keep the LS allocations */
    memset(in, 0, sizeof *in);
    memcpy(in->spu, spus, sizeof spus);
    in->ea = ea;
    in->nspus = nspus > KMAX_SPU ? KMAX_SPU : nspus;
    InitializeSRWLock(&in->lock);
    InitializeSRWLock(&in->hlock);
    InitializeConditionVariable(&in->idle_cv);
    InitializeConditionVariable(&in->hcv);
    InitializeConditionVariable(&in->scv);
    ReleaseSRWLockExclusive(&s_table);
    in->event_th = CreateThread(NULL, 1u << 20, kevent_main, in, 0, NULL);
    in->handler_th = CreateThread(NULL, 1u << 20, khandler_main, in, 0, NULL);
}

void spurs_kernel_notify(uint32_t ea)
{
    KInst* in = kinst_find(ea);
    if (!in) return;
    AcquireSRWLockExclusive(&in->lock);
    WakeAllConditionVariable(&in->idle_cv);
    ReleaseSRWLockExclusive(&in->lock);
}

void spurs_kernel_wakeup(uint32_t ea)
{
    KInst* in = kinst_find(ea);
    if (!in) return;
    uint8_t* S = B(ea);
    AcquireSRWLockExclusive(&in->hlock);
    S[H_DIRTY] = 1;
    if (S[H_WAITING]) WakeAllConditionVariable(&in->hcv);
    ReleaseSRWLockExclusive(&in->hlock);
    spurs_kernel_notify(ea);
}

void spurs_kernel_shutdown_completed(uint32_t ea, uint32_t wid)
{
    KInst* in = kinst_find(ea);
    if (in && wid < SPURS_MAX_WKL) kevent_notify(in, 0x80000000u >> wid);
}

void spurs_kernel_wait_shutdown_sema(uint32_t ea, uint32_t wid)
{
    KInst* in = kinst_find(ea);
    if (!in || wid >= SPURS_MAX_WKL) return;
    AcquireSRWLockExclusive(&in->hlock);
    while (!in->sema[wid])
        SleepConditionVariableSRW(&in->scv, &in->hlock, INFINITE, 0);
    in->sema[wid]--;
    ReleaseSRWLockExclusive(&in->hlock);
}

void spurs_kernel_stop(uint32_t ea)
{
    KInst* in = kinst_find(ea);
    if (!in) return;
    uint8_t* S = B(ea);
    AcquireSRWLockExclusive(&in->hlock);
    in->exiting = 1;
    S[H_EXITING] = 1;
    WakeAllConditionVariable(&in->hcv);
    WakeAllConditionVariable(&in->scv);
    ReleaseSRWLockExclusive(&in->hlock);
    /* ask every SPU to leave through its system service */
    AcquireSRWLockExclusive(&in->lock);
    __atomic_store_n(S + SPURS_SYSSRV_TERMINATE, 0xFF, __ATOMIC_SEQ_CST);
    __atomic_store_n(S + SPURS_SYSSRV_MESSAGE, 0xFF, __ATOMIC_SEQ_CST);
    WakeAllConditionVariable(&in->idle_cv);
    ReleaseSRWLockExclusive(&in->lock);
    if (in->handler_th) { WaitForSingleObject(in->handler_th, INFINITE); CloseHandle(in->handler_th); }
    if (in->event_th)   { WaitForSingleObject(in->event_th, INFINITE);   CloseHandle(in->event_th); }
    AcquireSRWLockExclusive(&s_table);
    in->ea = 0;
    in->handler_th = in->event_th = NULL;
    ReleaseSRWLockExclusive(&s_table);
}
