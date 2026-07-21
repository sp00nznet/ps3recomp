/*
 * ps3recomp - SPU (Synergistic Processing Unit) execution context
 *
 * Models the full architectural state of an SPU:
 *   - 128 x 128-bit general-purpose registers
 *   - 256 KB local store
 *   - Channel state (MFC command queue, mailboxes, signal notification)
 */

#ifndef SPU_CONTEXT_H
#define SPU_CONTEXT_H

#include "../../include/ps3emu/ps3types.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable 16-byte alignment (MSVC __declspec vs GCC/clang __attribute__). */
#if defined(_MSC_VER)
#  define SPU_ALIGN16 __declspec(align(16))
#else
#  define SPU_ALIGN16 __attribute__((aligned(16)))
#endif

/* Local store size: 256 KB */
#define SPU_LS_SIZE         (256 * 1024)
#define SPU_LS_MASK         (SPU_LS_SIZE - 1)

/* ---------------------------------------------------------------------------
 * Reusable env-driven LS watchpoint (mini-debugger, no rebuild to retarget).
 *   LBP_SPU_WATCH=0x1BE80        watch one 16-byte LS line (reads + writes)
 *   LBP_SPU_WATCH=0x1BE80,0x927D80  up to 4 comma-separated addresses
 * Fires from every SPU image's spu_ls_read128/write128. Cheap: one cached
 * compare on the hot path when disabled.
 * -----------------------------------------------------------------------*/
#define SPU_WATCH_MAX 4
static inline unsigned* spu_ls_watch_list(int* out_n) {
    static int init = 0; static unsigned addr[SPU_WATCH_MAX]; static int n = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("LBP_SPU_WATCH");
        while (e && *e && n < SPU_WATCH_MAX) {
            addr[n++] = (unsigned)strtoul(e, (char**)&e, 0) & ~0xFu;
            while (*e == ',' || *e == ' ') e++;
        }
    }
    *out_n = n;
    return addr;
}
static inline void spu_ls_watch_hit2(uint32_t lsa, int is_write, const uint8_t* p,
                                     uint32_t pc, uint32_t lr) {
    int n; unsigned* w = spu_ls_watch_list(&n);
    if (!n) return;
    uint32_t a = lsa & (SPU_LS_MASK & ~0xFu);
    for (int i = 0; i < n; i++) {
        if (w[i] == a) {
            fprintf(stderr, "[spu-watch %s 0x%05X pc=0x%05X lr=0x%05X] "
                "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                is_write ? "WR" : "rd", a, pc, lr,
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            fflush(stderr);
            break;
        }
    }
}
static inline void spu_ls_watch_hit(uint32_t lsa, int is_write, const uint8_t* p) {
    spu_ls_watch_hit2(lsa, is_write, p, 0, 0);
}

/* Maximum number of MFC tag groups */
#define SPU_MFC_MAX_TAGS    32

/* Mailbox / signal capacities */
#define SPU_MBOX_DEPTH      1    /* SPU write outbound mailbox depth */
#define SPU_INTR_MBOX_DEPTH 1    /* SPU write inbound interrupt mailbox depth */

/* ---------------------------------------------------------------------------
 * SPU channel IDs
 * -----------------------------------------------------------------------*/
#define SPU_RdEventStat     0
#define SPU_WrEventMask     1
#define SPU_WrEventAck      2
#define SPU_RdSigNotify1    3
#define SPU_RdSigNotify2    4
#define SPU_WrDec           7
#define SPU_RdDec           8
#define SPU_RdEventMask     11
#define SPU_RdMachStat      13
#define SPU_WrSRR0          14
#define SPU_RdSRR0          15
#define SPU_WrOutMbox       28
#define SPU_RdInMbox        29
#define SPU_WrOutIntrMbox   30

/* MFC channels */
#define MFC_WrMSSyncReq     9
#define MFC_RdTagMask       12
#define MFC_LSA             16
#define MFC_EAH             17
#define MFC_EAL             18
#define MFC_Size            19
#define MFC_TagID           20
#define MFC_Cmd             21
#define MFC_WrTagMask       22
#define MFC_WrTagUpdate     23
#define MFC_RdTagStat       24
#define MFC_RdListStallStat 25
#define MFC_WrListStallAck  26
#define MFC_RdAtomicStat    27

/* ---------------------------------------------------------------------------
 * MFC DMA command opcodes
 * -----------------------------------------------------------------------*/
#define MFC_PUT_CMD         0x20
#define MFC_PUTB_CMD        0x21
#define MFC_PUTF_CMD        0x22
#define MFC_GET_CMD         0x40
#define MFC_GETB_CMD        0x41
#define MFC_GETF_CMD        0x42
#define MFC_PUTL_CMD        0x24
#define MFC_PUTLB_CMD       0x25
#define MFC_PUTLF_CMD       0x26
#define MFC_GETL_CMD        0x44
#define MFC_GETLB_CMD       0x45
#define MFC_GETLF_CMD       0x46
#define MFC_SNDSIG_CMD      0xA0
#define MFC_BARRIER_CMD     0xC0
#define MFC_EIEIO_CMD       0xC8
#define MFC_SYNC_CMD        0xCC

/* Atomic (lock-line reservation) commands -- operate on a 128-byte line. */
#define MFC_GETLLAR_CMD     0xD0   /* get + reserve line                      */
#define MFC_PUTLLC_CMD      0xB4   /* store conditional (fails if line moved) */
#define MFC_PUTLLUC_CMD     0xB0   /* store unconditional, clears reservation */
#define MFC_PUTQLLUC_CMD    0xB8   /* queued store unconditional              */
#define MFC_ATOMIC_LINE     128

/* ---------------------------------------------------------------------------
 * Channel state
 * -----------------------------------------------------------------------*/
typedef struct spu_channel {
    uint32_t value;
    uint32_t count;   /* number of valid entries (0 or 1 for most channels) */
} spu_channel;

/* ---------------------------------------------------------------------------
 * SPU execution context
 * -----------------------------------------------------------------------*/
typedef struct spu_context {
    /* 128 general-purpose 128-bit registers */
    SPU_ALIGN16 u128 gpr[128];

    /* 256 KB local store, 16-byte aligned */
    SPU_ALIGN16 uint8_t ls[SPU_LS_SIZE];

    /* Program counter (local store address, 0-0x3FFFF) */
    uint32_t pc;

    /* SPU status (running, stopped, etc.) */
    uint32_t status;
    #define SPU_STATUS_STOPPED      0x0
    #define SPU_STATUS_RUNNING      0x1
    #define SPU_STATUS_STOPPED_BY_STOP  0x2
    #define SPU_STATUS_STOPPED_BY_HALT  0x4
    #define SPU_STATUS_WAITING_CHANNEL  0x8

    /* 14-bit signal code of the most recent `stop`/`stopd`. SPURS leaf tasks
     * invoke kernel syscalls via `stop <code>` (CELL_SPURS_TASK_SYSCALL_EXIT=0,
     * YIELD=1, WAIT_SIGNAL=2, POLL=3, RECV_WKL_FLAG=4; |0x10 = the v2 form), so
     * the taskset-PM emulation must read the code, not just halt. */
    uint32_t stop_code;
    #define SPU_STATUS_SINGLE_STEP  0x10

    /* SPU thread identification */
    uint32_t spu_id;
    uint32_t spu_group_id;

    /* Active recompiled image for per-context indirect-branch dispatch. SPURS
     * loads kernel/policy/job into overlapping LS addresses at different times,
     * so spu_indirect_branch resolves pc within the image currently selected
     * here. 0 = match any image (back-compat for single-image contexts). */
    int image_id;

    /* Decrementer: a free-running down counter, ticking at the PS3 timebase.
     * SPU_WrDec latches the reload value here and stamps dec_base_ns; SPU_RdDec
     * derives the live value from the host clock. Storing only the written
     * value (and returning it unchanged) makes every timed wait immortal --
     * see SPU_RdDec in spu_channels.c. */
    uint32_t decrementer;
    uint64_t dec_base_ns;

    /* SRR0 - Save/Restore Register (exception return address) */
    uint32_t srr0;

    /* Event status / mask */
    uint32_t event_status;
    uint32_t event_mask;

    /* Channels */
    spu_channel ch_out_mbox;        /* SPU -> PPU outbound mailbox */
    spu_channel ch_in_mbox;         /* PPU -> SPU inbound mailbox */
    spu_channel ch_out_intr_mbox;   /* SPU -> PPU interrupt mailbox */
    spu_channel ch_sig_notify[2];   /* Signal notification 1 & 2 */

    /* MFC command staging registers (written via channels before issuing cmd) */
    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;

    /* MFC tag completion mask and status */
    uint32_t mfc_tag_mask;
    uint32_t mfc_tag_status;

    /* Atomic reservation state (GETLLAR/PUTLLC lock-line semantics). Multiple
     * SPU kernel threads share a lock-free queue in main memory; PUTLLC must
     * FAIL if the reserved 128-byte line changed since GETLLAR, or concurrent
     * claims corrupt the queue. */
    uint32_t resv_ea;          /* reserved 128-byte line EA, aligned (0 = none) */
    int      resv_valid;
    uint32_t atomic_stat;      /* last atomic op result -> MFC_RdAtomicStat */
    uint8_t  resv_line[128];   /* snapshot of the line at GETLLAR time */

    /* SPURS policy-module run mode (spurs_policy.c): nonzero while a lifted
     * policy module runs under the HLE'd kernel handoff. spu_indirect_branch
     * intercepts branches to the two reserved kernel-service addresses below
     * only when this is set. */
    int policy_mode;

    /* Resident code-overlay image id (0 = none). FMOD's mixer swaps per-DSP
     * plugin code in and out of ONE LS scratch region (e.g. the Compressor
     * streams over the Fader plugin's slot), so one static per-image function
     * registry cannot hold both: their lifted addresses overlap. The MFC GET
     * path recognizes a registered overlay's source EA and records which
     * overlay is now resident; dispatch retries a missed lookup against it. */
    int resident_ovl;

    /* --- SPU_DRAIN trampoline execution model (faithful-adopt, canersaka) ---
     * host_depth counts live lifted call frames (matched brsl/bisl). SPU_RET
     * (`bi $r0`) does a host `return` while host_depth>0 (the caller's frame is
     * live); at 0 it dispatches to the link register instead (a cross-context
     * task resume / restack unwind killed the frame). module_img_a00 is the
     * persistent workload-module image adopted at LS 0xA00, re-applied at
     * dispatch after a call-bracket image restore. */
    uint32_t host_depth;
    int      module_img_a00;

} spu_context;

/* Reserved LS addresses (inside the kernel area, below the 0xA00 policy-module
 * base) that the HLE kernel plants as exitToKernelAddr / selectWorkloadAddr in
 * the SpursKernelContext. A policy module branching to them is performing a
 * kernel service; spu_indirect_branch intercepts (policy_mode only). */
#define SPURS_PM_EXIT_TO_KERNEL_LS   0x9C0u
#define SPURS_PM_SELECT_WORKLOAD_LS  0x9D0u

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void spu_context_init(spu_context* ctx, uint32_t spu_id)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->spu_id = spu_id;
    ctx->status = SPU_STATUS_STOPPED;
}

/* ---------------------------------------------------------------------------
 * Local store access helpers
 * -----------------------------------------------------------------------*/
static inline uint8_t* spu_ls_ptr(spu_context* ctx, uint32_t lsa)
{
    return &ctx->ls[lsa & SPU_LS_MASK];
}

static inline uint32_t spu_ls_read32(const spu_context* ctx, uint32_t lsa)
{
    lsa &= SPU_LS_MASK;
    const uint8_t* p = &ctx->ls[lsa];
    /* SPU local store is big-endian */
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void spu_ls_write32(spu_context* ctx, uint32_t lsa, uint32_t val)
{
    lsa &= SPU_LS_MASK;
    uint8_t* p = &ctx->ls[lsa];
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

/* Local-store quadword access.
 *
 * LS bytes are big-endian (the SPU's native order); registers in our
 * model are stored in native (little-endian on x86) order so that
 * `_u32[i]` directly gives lane i's value and `spu_preferred_u32(r)`
 * == `r._u32[0]`. The byte-swap below converts between the two on
 * every quadword load/store, mirroring what `spu_ls_read32` already
 * does explicitly. Doing the swap here (rather than in every channel
 * extractor) keeps the per-word `_u32[i]` semantics that the lifter
 * helpers in `spu_helpers.h` were written against. */
/* The 128-bit LS accessors are THE hot primitive of lifted SPU execution
 * (every load/store in a Bink movie frame's ~millions of lifted instructions
 * runs through here). On x86 the whole BE<->LE quadword swizzle is one
 * SSSE3 byte-shuffle instead of 16 scalar byte ops -- measured against the
 * scalar path with the same per-lane _u32 semantics. */
#if defined(_MSC_VER)
#include <stdlib.h>
#define SPU_BSWAP32(x) _byteswap_ulong(x)
#define SPU_LS_FAST 1
#elif defined(__GNUC__) || defined(__clang__)
#define SPU_BSWAP32(x) __builtin_bswap32(x)
#define SPU_LS_FAST 1
#endif

static inline u128 spu_ls_read128(const spu_context* ctx, uint32_t lsa)
{
    u128 v;
    lsa &= SPU_LS_MASK & ~0xFu;
    const uint8_t* p = &ctx->ls[lsa];
    spu_ls_watch_hit2(lsa, 0, p, (uint32_t)ctx->pc & SPU_LS_MASK,
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK);
#if SPU_LS_FAST
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, p,      4); memcpy(&w1, p + 4,  4);
    memcpy(&w2, p + 8,  4); memcpy(&w3, p + 12, 4);
    v._u32[0] = SPU_BSWAP32(w0); v._u32[1] = SPU_BSWAP32(w1);
    v._u32[2] = SPU_BSWAP32(w2); v._u32[3] = SPU_BSWAP32(w3);
#else
    for (int i = 0; i < 4; i++) {
        v._u32[i] = ((uint32_t)p[i*4]     << 24) |
                    ((uint32_t)p[i*4 + 1] << 16) |
                    ((uint32_t)p[i*4 + 2] <<  8) |
                    (uint32_t)p[i*4 + 3];
    }
#endif
    return v;
}

static inline void spu_ls_write128(spu_context* ctx, uint32_t lsa, u128 val)
{
    lsa &= SPU_LS_MASK & ~0xFu;
    uint8_t* p = &ctx->ls[lsa];
#if SPU_LS_FAST
    uint32_t w0 = SPU_BSWAP32(val._u32[0]), w1 = SPU_BSWAP32(val._u32[1]);
    uint32_t w2 = SPU_BSWAP32(val._u32[2]), w3 = SPU_BSWAP32(val._u32[3]);
    memcpy(p,      &w0, 4); memcpy(p + 4,  &w1, 4);
    memcpy(p + 8,  &w2, 4); memcpy(p + 12, &w3, 4);
#else
    for (int i = 0; i < 4; i++) {
        uint32_t w = val._u32[i];
        p[i*4]     = (uint8_t)(w >> 24);
        p[i*4 + 1] = (uint8_t)(w >> 16);
        p[i*4 + 2] = (uint8_t)(w >>  8);
        p[i*4 + 3] = (uint8_t)w;
    }
#endif
    spu_ls_watch_hit2(lsa, 1, p, (uint32_t)ctx->pc & SPU_LS_MASK,
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    /* SPU_SMC_WATCH=<img>: self-modification detector. Log any store whose
     * target LS line falls inside that image's CODE segment (the segment
     * bounds come from SPU_SMC_LO/HI, default the pm_wwsjob range 0xA00..
     * 0x3700). A hit proves the guest rewrites its own instructions -- which
     * a static recompiler cannot follow. */
    { static int s = -2; static uint32_t lo, hi, img;
      if (s == -2) { const char* e = getenv("SPU_SMC_WATCH");
        s = e ? atoi(e) : -1; img = (uint32_t)s;
        const char* l = getenv("SPU_SMC_LO"); lo = l ? (uint32_t)strtoul(l,0,0) : 0xA00;
        const char* h = getenv("SPU_SMC_HI"); hi = h ? (uint32_t)strtoul(h,0,0) : 0x3700; }
      if (s >= 0 && (uint32_t)ctx->image_id == img && lsa >= lo && lsa < hi) {
          static int _n = 0;
          if (_n++ < 48)
              fprintf(stderr, "[spu-SMC] img=%d WROTE CODE @0x%05X (pc=0x%05X) = %02X%02X%02X%02X\n",
                      ctx->image_id, lsa, (uint32_t)ctx->pc & SPU_LS_MASK,
                      p[0], p[1], p[2], p[3]);
      } }
}

/* ---------------------------------------------------------------------------
 * Preferred slot extraction
 *
 * SPU instructions operate on the "preferred slot" of a 128-bit register,
 * which is the leftmost (highest-address in big-endian) element.
 * For word operations, preferred slot = element 0 of the _u32 array
 * (since our u128 stores big-endian element order).
 * -----------------------------------------------------------------------*/
static inline uint32_t spu_preferred_u32(const u128* reg)
{
    return reg->_u32[0];
}

static inline int32_t spu_preferred_s32(const u128* reg)
{
    return reg->_s32[0];
}

static inline float spu_preferred_f32(const u128* reg)
{
    return reg->_f32[0];
}

static inline uint64_t spu_preferred_u64(const u128* reg)
{
    return reg->_u64[0];
}

/* Create a register with a value splatted to the preferred word slot */
static inline u128 spu_make_preferred_u32(uint32_t val)
{
    u128 r;
    memset(&r, 0, sizeof(r));
    r._u32[0] = val;
    return r;
}

/* ---------------------------------------------------------------------------
 * Channel read/write helpers
 * -----------------------------------------------------------------------*/
static inline void spu_channel_write(spu_channel* ch, uint32_t val)
{
    ch->value = val;
    ch->count = 1;
}

static inline uint32_t spu_channel_read(spu_channel* ch)
{
    uint32_t val = ch->value;
    ch->count = 0;
    return val;
}

static inline int spu_channel_has_data(const spu_channel* ch)
{
    return ch->count > 0;
}

/* ---------------------------------------------------------------------------
 * SPU_DRAIN trampoline execution model (faithful-adopt, from canersaka's fork).
 *
 * Replaces the musttail SPU_TAILCALL chain: a cross-function transfer sets
 * ctx->pc + g_spu_trampoline_fn and RETURNS (unwinds the host stack); the
 * enclosing SPU_DRAIN loop re-enters. This keeps the host stack bounded and
 * gives ONE central per-transfer hook (lockstep tick, later flight recorder).
 * The lifter (tools/spu_lifter.py) emits: calls as a bracketed nested host call
 * + SPU_DRAIN; tail/indirect branches as trampoline-set + return; `bi $r0` as
 * SPU_RET. Hooks below are STUBS for now (spu_drain.c) -- lockstep/fltrec land
 * in later milestones.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_THREAD_LOCAL __declspec(thread)
#else
#  define SPU_THREAD_LOCAL __thread
#endif

/* Indirect-branch dispatcher (spu_channels.c): resolves ctx->pc to a lifted
 * function in the active image and runs it. Referenced by SPU_RET/SPU_DRAIN. */
void spu_indirect_branch(spu_context* ctx);

/* Pending cross-function transfer target; NULL when none. Thread-local: each
 * spu_context is pinned to one host thread for its lifetime. */
extern SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);

/* Central per-transfer hooks (stubbed in spu_drain.c until their milestones). */
void yz_lockstep_tick(spu_context* ctx);             /* round-robin token gate */
void spu_task_launch_check(spu_context* ctx, void* fn); /* SPURS task-launch    */
/* Restore image_id after a call-bracket (adopt-on-serve); re-applies the
 * persistent LS-0xA00 workload module if one is set. */
void spu_img_restore(spu_context* ctx, int32_t saved_img);
/* Wake a host thread blocked in a channel wait (channel-stall milestone). */
void spu_ch_wake(spu_context* ctx);

/* Drain the pending trampoline chain: run each queued transfer target until
 * none remain. The one central hook site for the faithful execution model. */
#define SPU_DRAIN(ctx) do {                                    \
        while (g_spu_trampoline_fn) {                           \
            void (*_tf)(spu_context*) = g_spu_trampoline_fn;    \
            g_spu_trampoline_fn = 0;                            \
            yz_lockstep_tick(ctx);                             \
            spu_task_launch_check((ctx), (void*)_tf);          \
            _tf(ctx);                                          \
        }                                                      \
    } while (0)

/* Depth-aware SPU link return (`bi $r0`): host `return` while a matched
 * brsl/bisl frame is live (host_depth>0); at 0 the frame was destroyed (task
 * resume / restack unwind) so dispatch to the link register instead -- faithful
 * to CBEA `bi` (branch to word 0 of RA). pc set unconditionally. */
#define SPU_RET(ctx) do {                                      \
        (ctx)->pc = (ctx)->gpr[0]._u32[0];                     \
        if ((ctx)->host_depth == 0)                            \
            g_spu_trampoline_fn = spu_indirect_branch;         \
        return;                                                \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_CONTEXT_H */
