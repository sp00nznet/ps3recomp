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
 *   SPU_LS_WATCH=0x1BE80        watch one 16-byte LS line (reads + writes)
 *   SPU_LS_WATCH=0x1BE80,0x927D80  up to 4 comma-separated addresses
 * Fires from every SPU image's spu_ls_read128/write128. Cheap: one cached
 * compare on the hot path when disabled.
 * -----------------------------------------------------------------------*/
/* The check sits on every spu_ls_read128/write128, so the disabled case must
 * be one load and a branch: it was a real call (the function-local statics
 * kept it out of line) and ~9% of GH3's FMOD mixer task. The list lives in
 * spu_channels.c; g_spu_ls_watch_n is -1 until the first check reads the env. */
extern int g_spu_ls_watch_n;
struct spu_context;
void spu_ls_watch_slow(const struct spu_context* c, uint32_t lsa, int is_write,
                       const uint8_t* p, uint32_t pc, uint32_t lr);
/* SPU_LS_WATCH_RING fault-path dump: prints the recorded hits per watched
 * line, oldest first. Call from SPU fault handling (branch-to-0, bad op,
 * thread stop) -- the tail names the store that corrupted the LS. */
void spu_ls_watch_dump(const char* why);
unsigned* spu_ls_watch_list(int* out_n);   /* the armed lines (n may be 0) */
/* Ring-record a DMA landing on a watched line (ring mode only); returns 1 if
 * recorded, 0 when ring mode is off (caller prints). T-0001. */
int spu_ls_watch_ring_dma(struct spu_context* spu, uint32_t line, uint32_t ea,
                         uint32_t size, const uint8_t* q16);
static inline void spu_ls_watch_hit2(const struct spu_context* c, uint32_t lsa, int is_write,
                                     const uint8_t* p, uint32_t pc, uint32_t lr) {
    if (__builtin_expect(g_spu_ls_watch_n == 0, 1)) return;
    spu_ls_watch_slow(c, lsa, is_write, p, pc, lr);
}
static inline void spu_ls_watch_hit(uint32_t lsa, int is_write, const uint8_t* p) {
    spu_ls_watch_hit2(0, lsa, is_write, p, 0, 0);
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
/* Channel capacity. Most SPU channels are genuinely one deep, but the PPU->SPU
 * inbound mailbox is FOUR on hardware, and a title that sends a multi-word
 * message writes the words back to back after checking the free-slot count. A
 * one-deep channel drops all but one of them, and the SPU then blocks forever
 * on a message it half received -- silently, because nothing is wrong with
 * either side. The Orange Box sends CB.SPU descriptors of two words from one
 * call site and FIVE from another -- one more than the mailbox is deep, so a
 * whole message never fits at once and the sender has to poll free slots while
 * the SPU drains, exactly as on hardware. (The rcv_evt[4] queue below is the
 * same problem, solved once ad hoc for sys_spu_thread_receive_event's
 * four-word reply.) */
/* Capacity is NOT the hardware depth, deliberately.
 *
 * Hardware is four deep and back-pressures the writer: the PPU polls the
 * free-slot count and its store stalls until there is room. We cannot model
 * that half -- an MMIO store into the problem-state window is a plain memory
 * write on the host, already committed by the time the handler runs, so there
 * is no way to refuse it. Anything that does not fit is simply lost.
 *
 * Losing one word of a multi-word descriptor desynchronises the reader
 * permanently: it takes the words it did get, then waits for a word that has
 * already been and gone. The Orange Box sends CB.SPU bursts of two words and
 * of five -- one more than hardware depth -- and its worker reads a long
 * sequence across several call sites, so a single lost word strands it.
 *
 * So buffer generously and let the free-slot count keep advertising the
 * hardware depth (SPU_IN_MBOX_HW_DEPTH): a guest that polls sees the numbers it
 * expects, and a guest that does not poll still cannot lose data. */
#define SPU_CHANNEL_CAP      64
#define SPU_IN_MBOX_HW_DEPTH 4

/* Shared between the SPU and its producers/consumers on other host threads
 * (a PPU writing the inbound mailbox while the SPU reads it, the PPU polling
 * the outbound one). Every write and read takes `lock`; `count` is published
 * with release order so a reader that sees it non-zero also sees the word.
 * Read the fields through the spu_channel_* helpers below, never directly. */
typedef struct spu_channel {
    uint32_t value;   /* head: the value the next read returns */
    uint32_t count;   /* number of valid entries, 0..SPU_CHANNEL_CAP */
    uint32_t q[SPU_CHANNEL_CAP];
    uint32_t head;
    uint32_t lock;    /* spinlock over value/count/q/head */
} spu_channel;

/* ---------------------------------------------------------------------------
 * SPU execution context
 * -----------------------------------------------------------------------*/
typedef struct spu_context {
    /* 128 general-purpose 128-bit registers */
    SPU_ALIGN16 u128 gpr[128];

    /* 256 KB local store, 16-byte aligned, addressed through `ls`.
     *
     * Indirect because a RAW SPU's local store is not private: lv2 maps it into
     * the process at 0xE0000000 + n*0x100000, and the PPU writes the SPU's code
     * and its command buffers there WHILE IT RUNS (runtime/spu/spu_raw.c). Such
     * a context points `ls` straight at guest memory; every other context points
     * it at ls_store, so no other path changes -- ctx->ls[i], &ctx->ls[i] and
     * ctx->ls + n all still compile and mean the same thing.
     *
     * A context memset without spu_context_init would leave this NULL.
     * spurs_policy.c memsets and calls the init immediately after; keep it so. */
    SPU_ALIGN16 uint8_t ls_store[SPU_LS_SIZE];
    uint8_t* ls;

    /* Program counter (local store address, 0-0x3FFFF) */
    uint32_t pc;

    /* SPU status (running, stopped, etc.) */
    uint32_t status;
    /* Set by another thread (sys_spu_thread_group_terminate) to stop this SPU:
     * checked by blocking channel waits, interpreter steps and lifted
     * trampoline transfers, which then leave the SPU stopped. */
    uint32_t stop_request;
    /* Test hook (T-0001): when >0, spu_interp_run_until bails out after this
     * many steps with status still RUNNING -- lets the regression suite run
     * the never-exiting hang families (count % 4 != 0) against real guest
     * bytes without looping forever. */
    uint32_t interp_step_budget;
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

    /* When set, a rchcnt(SPU_RdInMbox) that finds the inbound mailbox EMPTY halts
     * the SPU (spu_halt longjmp) instead of returning 0. Lets a persistent-worker
     * SPU (e.g. the Rubber Ducky AsyncCopy raw SPU) run SYNCHRONOUSLY: it does its
     * full init + ready-mailbox handshake, then parks the first time it idle-waits
     * for a PPU command -- no async host thread racing the PPU. */
    int park_on_empty_inmbox;

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
    /* sys_spu_thread_receive_event (stop 0x110) replies with FOUR values --
     * {CELL_OK, data1, data2, data3} -- and the worker reads them back with
     * four rdch SPU_RdInMbox. spu_channel holds one, so queue the reply here
     * and let the in-mailbox read drain it first. */
    uint32_t rcv_evt[4];
    int      rcv_evt_n;             /* remaining unread reply words */
    int      rcv_evt_i;
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
    uint64_t dbg_getllar;      /* SPU_PUTLLC_WHY: GETLLARs issued      */
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
    /* Drain ticks left before a stall-and-notify interrupt may be taken (see
     * spu_take_interrupt). Per context: it was one global slot, so with
     * several SPU host threads one SPU's deferral overwrote another's and an
     * interrupt was lost. */
    unsigned sn_defer;
    /* Pending cross-function transfer target (g_spu_trampoline_fn); NULL when
     * none. Was a thread-local variable: every lifted block that leaves its
     * function writes it and the drain loop reads it, and on macOS each
     * thread-local access is a call (_tlv_get_addr) -- ~7% of SPU time. */
    void (*tramp_fn)(struct spu_context*);
    /* Independently streamed code buffers can coexist with the policy overlay.
     * Each mapping records which translated image owns that local-store span. */
    struct {
        uint32_t lsa, size, source_ea;
        int image_id;
    } resident_code[4];

    /* Resident SPURS-taskset TASK image id (0 = none) retires the id-0 wildcard
     * for co-resident tasks. A taskset may hold several tasks that all lift at the
     * SAME LS base (the shared task-code region at LS 0x3000+), so an LS address
     * alone cannot say which one owns it. Outside the region it is cleared;
     * on entry/resume it is resolved from the resident policy's TaskInfo ELF
     * registration, or the legacy entry map if no ELF metadata is available.
     * This also handles returns to internal PCs after scheduler calls. */
    int      resident_task;

    /* --- SPU_DRAIN trampoline execution model (faithful-adopt, canersaka) ---
     * host_depth counts live lifted call frames (matched brsl/bisl). SPU_RET
     * (`bi $r0`) does a host `return` while host_depth>0 (the caller's frame is
     * live); at 0 it dispatches to the link register instead (a cross-context
     * task resume / restack unwind killed the frame). module_img_a00 is the
     * persistent workload-module image adopted at LS 0xA00, re-applied at
     * dispatch after a call-bracket image restore. */
    uint32_t host_depth;
    /* spu_depth_guard's ring of recent drain sites (reported when the depth
     * trips). Per context rather than thread-local: on macOS every
     * thread-local access is a _tlv_get_addr call, and this runs on every
     * lifted call return. */
    uint32_t depth_ring[24];
    uint32_t depth_ring_n;
    /* The last 8 dispatched PCs (SPU_DRAIN step), for the resolver's
     * unlifted-branch report. Per context for the same reason. */
    uint32_t pch[8];
    uint32_t pch_n;

    /* Trampoline steps taken since this context started running. Only needed
     * to tell a job's INITIAL entry at LS 0 from a later return to LS 0, which
     * means something quite different (see spu_task_launch_check). */
    uint32_t steps;
    int      module_img_a00;

    /* SPU lockstep gate (spu_lockstep.c; env SPU_LOCKSTEP, default off).
     * quantum_ctr counts tick sites toward a token handoff; release_tb stamps
     * the guest-timebase moment this ctx last released the token. dec_start_tb
     * is the lockstep decrementer-freeze anchor (written only while armed). */
    uint64_t lockstep_quantum_ctr;
    uint64_t lockstep_release_tb;
    uint64_t dec_start_tb;

    /* Channel-stall contract (spu_channels.c): a blocking spu_rdch on an empty
     * RdInMbox/RdSigNotify/RdEventStat parks the SPU host thread on this per-SPU
     * CV until a producer (mailbox/signal write, event raise) calls spu_ch_wake.
     * Storage for a Win32 CONDITION_VARIABLE + SRWLOCK -- each is pointer-sized
     * and zero-init is the valid initial value (CONDITION_VARIABLE_INIT /
     * SRWLOCK_INIT == {0}), so spu_context_init's memset is enough. Kept as
     * void* to avoid pulling <windows.h> into this widely-included header;
     * spu_channels.c casts to the real types. */
    void* ch_wait_cv;
    void* ch_wait_lock;

    /* --- SPU interrupt facility (faithful; WWS jobmanager depends on it) ---
     * The bi-family E/D bits (bie/bid, irete/iretd...) toggle int_enable; while
     * enabled, a pending (event_status & event_mask) vectors execution to LS 0
     * at the next trampoline transfer (SPU_DRAIN hook -> spu_take_interrupt):
     * srr0 <- interrupted pc, int_enable <- 0, pc <- target of the guest-planted
     * branch instruction AT LS 0 (pm_wwsjob's entry writes `bra 0xA2C` there).
     * The handler returns via iret (pc <- srr0). */
    uint32_t int_enable;
    /* The host frame that took the pending interrupt (spu_drain_call's loop),
     * innermost first. The WWS handler leaves through a routine it assembles
     * at the top of local store, from wherever its state machine happens to
     * be -- several lifted calls deep -- so the iret rarely returns to the
     * frame that entered the handler. When the iret's register restore fires
     * deeper than the taking frame, execution unwinds to that frame and goes
     * on at srr0 from there; the handler's host frames hold nothing the guest
     * still needs. Without it the interrupted code resumed INSIDE the
     * handler's frames and its return address then missed every drain on the
     * way up (drain-mismatch return_pc=0xA90 pc=0x2D28, once per job). */
    void*    irq_frame;

    /* Parked DMA-list commands (stall-and-notify), keyed PER TAG GROUP. A
     * GETL/PUTL that transfers a list element with the stall-and-notify bit
     * (bit 31 of word0) set raises SPU event 0x2 (SPU_EVENT_SN) and parks the
     * remainder here; MFC_WrListStallAck for the tag resumes it.
     *
     * The WWS job manager pipelines Load/Run/Store stages, each issuing its own
     * barriered-null-list interrupt on a DISTINCT tag (readCommands=0,
     * writeShareableBuffers=3, writeAllShareableBuffers=5, writeJobBuffers=6 --
     * see jobmanagerdmatags.h), so multiple stalls can be outstanding at once.
     * A single-slot park dropped all but the last -> a pipeline stage's
     * completion interrupt was lost -> the job ring deadlocked. Hardware (and
     * RPCS3) accumulate: ch_stall_stat ORs (1<<tag) per stall; rdch returns the
     * accumulated mask then clears it; ch_stall_mask tracks parked tags and is
     * cleared per-tag by WrListStallAck. Modelled here as per-tag arrays. */
    uint32_t list_stall_mask;             /* bit t set = tag t has a parked list */
    uint32_t list_stall_stat;             /* readable notify mask (rdch clears) */
    uint32_t list_stall_elem_lsa[32];     /* LS addr of NEXT element, per tag */
    uint32_t list_stall_remaining[32];    /* elements left after stall, per tag */
    uint32_t list_stall_dest_lsa[32];     /* accumulated destination LSA, per tag */
    uint64_t list_stall_ea_base[32];      /* ea base (hi32 carries), per tag */
    uint32_t list_stall_cmd[32];          /* base (non-list) MFC command, per tag */

    /* Register file saved when an interrupt is taken, restored at its iret
     * (spu_drain.c). Per context: it used to be an 8-slot global table claimed
     * without a lock, and two SPU threads taking interrupts at once could take
     * the same slot -- one lost its save and returned from the handler with
     * the handler's registers. */
    int      irq_saved;
    uint32_t irq_resume_pc;
    uint64_t irq_save_steps;   /* T-0001: ctx->steps at take, for stale-restore detection */
    u128     irq_gpr[128];

    /* Return pc of the innermost spu_drain_call (0 = none). The interpreter
     * rejoins there: a return point sits mid-function, so it is no lifted
     * entry, and without this the interpreter ran a job's return straight on
     * through the caller's code until the next lifted entry -- GH3's job
     * manager then ran its buffer loop a second time on the job's registers
     * and died in its own assert (the song-freeze). */
    uint32_t drain_ret_pc;
    /* The SPURS kernel instance running on this SPU (libs/spurs/spurs_kernel.c),
     * or NULL: a policy module's selectWorkload call is answered by it. */
    void* spurs_vspu;
} spu_context;

/* The SPURS kernel's own service entry points, which it plants as
 * exitToKernelAddr / selectWorkloadAddr in the SpursKernelContext: the
 * addresses firmware libsre's kernel uses (read back by a policy module under
 * libsre, tests/conformance/spurs/t_workload). A policy module branching to
 * them is performing a kernel service; spu_indirect_branch intercepts
 * (policy_mode only). */
#define SPURS_PM_EXIT_TO_KERNEL_LS   0x808u
#define SPURS_PM_SELECT_WORKLOAD_LS  0x290u

/* The taskset policy module's syscall trampoline. Both the full resolver
 * (spu_channels.c) and the musttail fast path (spu_dispatch_mt.c) must treat a
 * branch here as special, so the address lives here rather than being #defined
 * twice -- it was, and the second copy carried a "mirrors the other one"
 * comment, which is a drift bug waiting for someone to change one of them. */
#define SPURS_TASKSET_PM_SYSCALL_LS  0xA70u

/* Image whose dispatch must always reach the full resolver: the CRI audio
 * image needs the resolver's r4 fixup, which the fast path cannot do without
 * becoming the resolver. Named here for the same reason as the address above.
 * ponytail: still a per-title magic id -- the durable fix is residency-aware
 * dispatch (issue #59), which subsumes this. */
#define SPU_RESOLVER_ONLY_IMAGE_ID   23

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void spu_context_init(spu_context* ctx, uint32_t spu_id)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->ls     = ctx->ls_store;   /* a raw SPU repoints this at guest memory */
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

/* Debug probes of spu_ls_read128, out of line: inlined they kept every LS
 * load a real call (~10% of GH3's FMOD mixer task). g_spu_ls_probe is -1
 * until the first call reads SPU_LS_LOWREAD, then 0/1. */

/* The diagnostic gates the inline helpers below read. Defined here rather than
 * in spu_channels.c, as one merged definition per program (selectany / weak),
 * so every target that includes this header links whether or not it builds
 * spu_channels.c -- the SPU unit tests and sync_stress do not. */
#if defined(_MSC_VER)
#  define SPU_GATE __declspec(selectany)
#else
#  define SPU_GATE __attribute__((weak))
#endif
SPU_GATE int g_spu_ls_probe   = -1;  /* spu_ls_read_probe (SPU_LS_LOWREAD)      */
SPU_GATE int g_spu_smc_watch  = -1;  /* spu_ls_write_probe_smc (SPU_SMC_WATCH)  */
SPU_GATE int g_wws_code_probe = 0;   /* WWS code-buffer resolution, capped      */
SPU_GATE int g_wws_read_probe = 0;
extern int g_spu_ls_dbg;     /* any LS debug hook armed (spu_channels.c) */
static __attribute__((noinline, cold)) void spu_ls_read_probe(const spu_context* ctx, uint32_t lsa)
{
    /* SPU_LS_LOWREAD=1: a job that reads its OWN first bytes as data is
     * dereferencing a null base -- the job binary loads at LS 0, so [NULL+off]
     * returns its own instruction words. Report each distinct low address once,
     * with the pc, to find which pointer was never filled in. */
    { static _Atomic int s_lw = -1;
      if (s_lw < 0) s_lw = getenv("SPU_LS_LOWREAD") ? 1 : 0;
      if (s_lw && lsa < 0x200u && ctx->image_id > 0 && !ctx->policy_mode) {
          static uint32_t seen[24]; static int n = 0; int known = 0;
          uint32_t key = (uint32_t)ctx->image_id << 24 | (lsa & 0x1F0);
          for (int i = 0; i < n; i++) if (seen[i] == key) { known = 1; break; }
          if (!known && n < 24) {
              seen[n++] = key;
              fprintf(stderr, "[spu-lowread] img=%d read LS 0x%03X at pc=0x%05X "
                      "ctx=%p r1=0x%05X r2=0x%08X r3=0x%08X r4=0x%08X\n",
                      ctx->image_id, lsa, (uint32_t)ctx->pc & SPU_LS_MASK,
                      (const void*)ctx,                      ctx->gpr[1]._u32[0], ctx->gpr[2]._u32[0],
                      ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
              fflush(stderr);
          }
      } }
    /* WWS buffer-resolution probe: GetLogicalBuffer reads bufferSetArray at
     * 0xDF0 + (jobNum<<6) + (logBufSet<<2), so the raw address encodes both.
     * Log them (capped) to see which set each command -- especially RunJob --
     * resolves to. Uses the RAW lsa before 16-byte alignment masking. */
    if (ctx->image_id == 2 && ctx->policy_mode && lsa >= 0xDF0 && lsa < 0xEB0) {
        extern int g_wws_read_probe;
        if (g_wws_read_probe < 48) {
            g_wws_read_probe++;
            uint32_t off = lsa - 0xDF0;
            fprintf(stderr, "[wws-res] bufferSetArray read jobNum=%u logBufSet=%u off=0x%X\n",
                    off >> 6, (off >> 2) & 0xF, off);
        }
    }
}

static inline u128 spu_ls_read128(const spu_context* ctx, uint32_t lsa)
{
    u128 v;
    if (__builtin_expect(g_spu_ls_dbg != 0, 0)) {
        spu_ls_read_probe(ctx, lsa);
        spu_ls_watch_hit2(ctx, lsa & (SPU_LS_MASK & ~0xFu), 0, &ctx->ls[lsa & (SPU_LS_MASK & ~0xFu)],
                          (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    }
    lsa &= SPU_LS_MASK & ~0xFu;
    const uint8_t* p = &ctx->ls[lsa];
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

static __attribute__((noinline, cold)) void spu_ls_write_probe_pre(spu_context* ctx, uint32_t lsa, u128 val)
{
    if (lsa == 0x1320 && ctx->image_id == 2 && ctx->policy_mode) {
        extern int g_wws_code_probe;   /* defined in spu_channels.c, capped */
        if (g_wws_code_probe < 6) {
            g_wws_code_probe++;
            uint32_t codeLsa = val._u32[0];
            #define _RD32(o) (((uint32_t)ctx->ls[o]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)| \
                              ((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
            uint32_t loadJob = _RD32(0x12D0), nextLoad = _RD32(0x12C0), runJob = _RD32(0x12E0);
            /* GetLogicalBuffer keys the bufferSet on jobNum(=loadJobNum): row at
             * 0xDF0 + jobNum*64. Dump THAT row (and job0's) so we see which set
             * the code resolves from. codeLsa>>10 = the resolved page. */
            fprintf(stderr, "[wws-code] codeLS=0x%05X (page 0x%X) loadJobNum=0x%X nextLoad=0x%X runJobNum=0x%X\n",
                    codeLsa & 0x3FFFF, (codeLsa>>10)&0xFF, loadJob, nextLoad, runJob);
            uint32_t row = (loadJob <= 3) ? (0xDF0 + loadJob*64) : 0xDF0;
            const uint8_t* bs = &ctx->ls[row];
            fprintf(stderr, "  bufferSetArray[job=0x%X @0x%X]:", loadJob, row);
            for (int i = 0; i < 32; i += 4)
                fprintf(stderr, " %02X%02X%02X%02X", bs[i],bs[i+1],bs[i+2],bs[i+3]);
            const uint8_t* lc = &ctx->ls[0xC00];
            fprintf(stderr, "\n  loadCommands[0xC00]:");
            for (int i = 0; i < 0x80; i += 4)
                fprintf(stderr, " %02X%02X%02X%02X", lc[i],lc[i+1],lc[i+2],lc[i+3]);
            fprintf(stderr, "\n");
            #undef _RD32
        }
    }
}
/* SPU_SMC_WATCH, out of line: g_spu_smc_watch is -1 until read, then 0/1. */
extern int g_spu_smc_watch;
static __attribute__((noinline, cold)) void spu_ls_write_probe_smc(spu_context* ctx, uint32_t lsa, const uint8_t* p)
{
    if (g_spu_smc_watch < 0) g_spu_smc_watch = getenv("SPU_SMC_WATCH") ? 1 : 0;
    /* SPU_SMC_WATCH=<img>: self-modification detector. Log any store whose
     * target LS line falls inside that image's CODE segment (the segment
     * bounds come from SPU_SMC_LO/HI, default the pm_wwsjob range 0xA00..
     * 0x3700). A hit proves the guest rewrites its own instructions -- which
     * a static recompiler cannot follow. */
    { static _Atomic int s = -2; static _Atomic uint32_t lo, hi, img;
      if (s == -2) { const char* e = getenv("SPU_SMC_WATCH");
        s = e ? atoi(e) : -1; img = (uint32_t)s;
        const char* l = getenv("SPU_SMC_LO"); lo = l ? (uint32_t)strtoul(l,0,0) : 0xA00;
        const char* h = getenv("SPU_SMC_HI"); hi = h ? (uint32_t)strtoul(h,0,0) : 0x3700; }
      if (s >= 0 && (uint32_t)ctx->image_id == img && lsa >= lo && lsa < hi) {
          static int _n = 0;
          if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 48)
              fprintf(stderr, "[spu-SMC] img=%d WROTE CODE @0x%05X (pc=0x%05X) = %02X%02X%02X%02X\n",
                      ctx->image_id, lsa, (uint32_t)ctx->pc & SPU_LS_MASK,
                      p[0], p[1], p[2], p[3]);
      } }
}

static inline void spu_ls_write128(spu_context* ctx, uint32_t lsa, u128 val)
{
    lsa &= SPU_LS_MASK & ~0xFu;
    uint8_t* p = &ctx->ls[lsa];
    /* WWS code-buffer probe: LBP's ChangeLoadToRunJob dispatches to LS[0x1320]
     * (= lsaJobCodeBuffer in LBP's layout) + entryOffset. When TecRunJob writes
     * that code-buffer LS address, dump the RunJob decision inputs so we can see
     * why it resolves to an empty buffer: the resolved address, the whole
     * bufferSetArray (0xDF0), and the live loadCommands (0xC00) whose RunJob
     * command (commandNum==5) names the code buffer set. */
    if (__builtin_expect(g_spu_ls_dbg != 0, 0) && lsa == 0x1320 && ctx->image_id == 2 && ctx->policy_mode)
        spu_ls_write_probe_pre(ctx, lsa, val);
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
    if (__builtin_expect(g_spu_ls_dbg != 0, 0)) {
        spu_ls_watch_hit2(ctx, lsa, 1, p, (uint32_t)ctx->pc & SPU_LS_MASK,
                          ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        if (g_spu_smc_watch) spu_ls_write_probe_smc(ctx, lsa, p);
    }
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
/* `value` stays the head, and `count` the number of entries, so the many places
 * that read those two fields directly keep working unchanged.
 *
 * Write and read are serialised by a per-channel spinlock. A mailbox has its
 * producer and consumer on different host threads (PPU MMIO store vs the SPU's
 * rdch), and unlocked `count++` / `count--` lose updates: Tornado Outbreak's
 * raw SPU consumed one mailbox word while the PPU queued the next, count landed
 * on 0 with a word still queued, and the PPU waited forever for a reply to a
 * command the SPU never saw. The reader could also see the new count before
 * the new value. Plain reads of `count` elsewhere stay lock-free. */
/* SPU event status: raised by other threads (a PPU store breaking a
 * reservation, an MFC list stall) while the SPU reads and acknowledges it.
 * Atomic at every access, so a raise can neither be lost to the SPU's
 * acknowledge nor be seen half-done. */
#define spu_ev_get(c)        __atomic_load_n(&(c)->event_status, __ATOMIC_ACQUIRE)
#define spu_ev_raise(c, b)   ((void)__atomic_fetch_or(&(c)->event_status, (uint32_t)(b), __ATOMIC_RELEASE))
#define spu_ev_ack(c, b)     ((void)__atomic_fetch_and(&(c)->event_status, ~(uint32_t)(b), __ATOMIC_ACQ_REL))

static inline void spu_channel_lock(spu_channel* ch)
{
    while (__atomic_exchange_n(&ch->lock, 1u, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&ch->lock, __ATOMIC_RELAXED)) { }
    }
}
static inline void spu_channel_unlock(spu_channel* ch)
{
    __atomic_store_n(&ch->lock, 0u, __ATOMIC_RELEASE);
}

#define SPU_CH_LOCK(ch)   spu_channel_lock(ch)
#define SPU_CH_UNLOCK(ch) spu_channel_unlock(ch)

/* Entries waiting. Safe from any thread; a non-zero answer means a following
 * spu_channel_read finds the word. */
static inline uint32_t spu_channel_count(const spu_channel* ch)
{
    return __atomic_load_n(&ch->count, __ATOMIC_ACQUIRE);
}

/* The word the next read returns, without consuming it (0 when empty). */
static inline uint32_t spu_channel_peek(spu_channel* ch)
{
    spu_channel_lock(ch);
    uint32_t v = ch->count ? ch->value : 0u;
    spu_channel_unlock(ch);
    return v;
}

static inline void spu_channel_clear(spu_channel* ch)
{
    spu_channel_lock(ch);
    ch->head = 0;
    __atomic_store_n(&ch->count, 0u, __ATOMIC_RELEASE);
    spu_channel_unlock(ch);
}

static inline void spu_channel_write(spu_channel* ch, uint32_t val)
{
    spu_channel_lock(ch);
    if (ch->count >= SPU_CHANNEL_CAP) {
        /* Full. Hardware does not accept the write at all -- the sender polls
         * the free-slot count first -- so the NEW word is what is lost.
         *
         * Dropping the oldest instead silently REORDERS the queue, which is
         * worse than losing a word: a reader taking a fixed-length message off
         * the mailbox then gets a prefix of one message spliced to the tail of
         * the next, and every field after the splice is garbage that still
         * looks plausible. The Orange Box sends CB.SPU a five-word descriptor
         * from five consecutive call sites -- one more than the mailbox is
         * deep -- so it meets this on every send. */
        spu_channel_unlock(ch);
        return;
    }
    ch->q[(ch->head + ch->count) % SPU_CHANNEL_CAP] = val;
    ch->value = ch->q[ch->head];
    __atomic_store_n(&ch->count, ch->count + 1u, __ATOMIC_RELEASE);
    spu_channel_unlock(ch);
}

static inline uint32_t spu_channel_read(spu_channel* ch)
{
    spu_channel_lock(ch);
    uint32_t val = ch->value;
    if (ch->count) {
        ch->head = (ch->head + 1u) % SPU_CHANNEL_CAP;
        if (ch->count > 1u) ch->value = ch->q[ch->head];
        __atomic_store_n(&ch->count, ch->count - 1u, __ATOMIC_RELEASE);
    }
    spu_channel_unlock(ch);
    return val;
}

/* Signal-notification OR mode: a write merges into the pending word, or is
 * the word when none is pending. One locked step, so a concurrent read
 * either sees the merged word or takes the old one and leaves the new. */
static inline void spu_channel_or(spu_channel* ch, uint32_t val)
{
    spu_channel_lock(ch);
    if (ch->count) {
        ch->q[ch->head] |= val;
        ch->value = ch->q[ch->head];
    } else {
        ch->q[ch->head] = val;
        ch->value = val;
        __atomic_store_n(&ch->count, 1u, __ATOMIC_RELEASE);
    }
    spu_channel_unlock(ch);
}

/* A PPU write to an SPU's inbound mailbox: four entries, and a write to a full
 * mailbox replaces the newest entry rather than failing or waiting (RPCS3 and
 * the CBEA agree; the mcx suite's X2 pins it). */
static inline void spu_channel_push_inmbox(spu_channel* ch, uint32_t val)
{
    spu_channel_lock(ch);
    if (ch->count < SPU_IN_MBOX_HW_DEPTH) {
        ch->q[(ch->head + ch->count) % SPU_CHANNEL_CAP] = val;
        ch->value = ch->q[ch->head];
        __atomic_store_n(&ch->count, ch->count + 1u, __ATOMIC_RELEASE);
    } else {
        ch->q[(ch->head + ch->count - 1u) % SPU_CHANNEL_CAP] = val;
        ch->value = ch->q[ch->head];
    }
    spu_channel_unlock(ch);
}

/* A signal-notification register in overwrite mode: one word, replaced. */
static inline void spu_channel_overwrite(spu_channel* ch, uint32_t val)
{
    spu_channel_lock(ch);
    ch->q[ch->head] = val;
    ch->value = val;
    __atomic_store_n(&ch->count, 1u, __ATOMIC_RELEASE);
    spu_channel_unlock(ch);
}

static inline int spu_channel_has_data(const spu_channel* ch)
{
    return spu_channel_count(ch) > 0;
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

/* Non-local exits in the SPU runtime are plain control flow (halt, interrupt
 * return), never out of a signal handler, so skip the signal-mask save that
 * setjmp does on Darwin/BSD -- a sigprocmask system call on every SPU entry. */
#if defined(_WIN32)
#  define SPU_SETJMP(env)     setjmp(env)
#  define SPU_LONGJMP(env, v) longjmp(env, v)
#else
#  define SPU_SETJMP(env)     _setjmp(env)
#  define SPU_LONGJMP(env, v) _longjmp(env, v)
#endif

/* Indirect-branch dispatcher (spu_channels.c): resolves ctx->pc to a lifted
 * function in the active image and runs it. Referenced by SPU_RET/SPU_DRAIN. */
void spu_indirect_branch(spu_context* ctx);

/* Pending cross-function transfer target; NULL when none. Lives in the
 * context (see spu_context.tramp_fn); every use site has `ctx` in scope. */
#define g_spu_trampoline_fn ((ctx)->tramp_fn)

/* Ring of the last PCs this SPU thread executed, for the unlifted-branch
 * report. The dispatcher records indirect branches; SPU_DRAIN records every
 * trampoline hop, which is the one that actually precedes a bad branch. */

/* Central per-transfer hooks (stubbed in spu_drain.c until their milestones). */
void yz_lockstep_tick(spu_context* ctx);             /* round-robin token gate */
void spu_task_launch_check(spu_context* ctx, void* fn); /* SPURS task-launch    */
/* Restore image_id after a call-bracket (adopt-on-serve); re-applies the
 * persistent LS-0xA00 workload module if one is set. */
void spu_img_restore(spu_context* ctx, int32_t saved_img);
/* Wake a host thread blocked in a channel wait (channel-stall milestone). */
void spu_ch_wake(spu_context* ctx);
/* Runaway lifted-call recursion guard. A lifted brsl/bisl nests a real host
 * call frame, so an SPU loop the lifter lowered as a CALL rather than a
 * transfer recurses until the host stack dies -- surfacing as a bare
 * STACKOVERFLOW backtrace of unsymbolised RVAs that says nothing about which
 * SPU code is looping. Report the SPU pc/image once and halt instead.
 * SPU_HOST_DEPTH_MAX overrides the limit. */
void spu_depth_guard(spu_context* ctx);
int  spu_tailret_enabled(void);
/* Take a pending SPU interrupt (spu_drain.c): srr0 <- ctx->pc, int_enable <- 0,
 * decode the guest-planted branch at LS 0 into ctx->pc, and return the
 * dispatcher to run instead of the interrupted transfer. Returns `tf`
 * unchanged (interrupt not taken) if LS 0 holds no branch instruction. */
void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*);

/* Drain a translated call up to its explicit architectural return PC. */
void spu_drain_call(spu_context* ctx, uint32_t return_pc);
/* SPU_PCHIST=1: bucket SPU execution by the GPU module's own symbols.
 *
 * SPU_VRAMPC showed 3.7M VRAM writes from Host2Local_Body and ZERO from
 * DrawRect/DrawEdge, i.e. the rasteriser never writes a pixel. That leaves two
 * possibilities -- the draw commands never arrive, or they arrive and main
 * never dispatches them -- and the difference is whether DrawRect/DrawEdge are
 * ENTERED at all. This counts entries rather than writes, one increment per
 * trampoline hop, which is the cheapest place that sees every transfer of
 * control.
 *
 * Extents from the .symtab embedded in ps1_netemu (firmware file offset
 * 0x171100), so they are exact rather than guessed. */
static inline void spu_pchist_tick(const spu_context* ctx)
{
    static _Atomic int en = -1;
    if (en < 0) en = getenv("SPU_PCHIST") ? 1 : 0;
    if (!en) return;
    static unsigned long long b[8][8], n;
    const unsigned sp = (unsigned)(ctx->spu_id & 7u);
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    const int k = pc < 0x00188u ? 0
                : pc < 0x00350u ? 1     /* BlockClear */
                : pc < 0x00758u ? 2     /* Host2Local_Body */
                : pc < 0x00928u ? 3     /* Host2Local */
                : pc < 0x010F8u ? 4     /* Local2Local */
                : pc < 0x051E8u ? 5     /* main */
                : pc < 0x08700u ? 6     /* DrawRect */
                : pc < 0x1493Cu ? 7     /* DrawEdge */
                : 0;
    b[sp][k]++;
    if ((++n % 4000000ull) == 0) {
        static const char* nm[8] = { "other", "BlockClear", "H2L_Body",
            "Host2Local", "Local2Local", "main", "DrawRect", "DrawEdge" };
        fprintf(stderr, "[pchist] %llu hops\n", n);
        for (unsigned q = 0; q < 8; q++) {
            int any = 0;
            for (int j = 0; j < 8; j++) if (b[q][j]) any = 1;
            if (!any) continue;
            fprintf(stderr, "   spu%u", q);
            for (int j = 0; j < 8; j++)
                if (b[q][j]) fprintf(stderr, " %s=%llu", nm[j], b[q][j]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
}

/* Drain the pending trampoline chain: run each queued transfer target until
 * none remain. The one central hook site for the faithful execution model. */
#define SPU_DRAIN(ctx) do {                                    \
        spu_depth_guard(ctx);                                  \
        while (g_spu_trampoline_fn) {                           \
            /* SPU_TAILRET=1: a pending transfer whose target equals the   \
             * link register IS a return. SPU_RET only recognises `bi $r0`;\
             * code returning through another register (`bi $r6`, r6 = the \
             * link) lowers as a transfer, so the host frame opened by the \
             * matching brsl/bisl never pops and a poll loop nests a frame \
             * per iteration until the host stack dies. Off by default --  \
             * this changes core dispatch for every title. */              \
            if (spu_tailret_enabled() && (ctx)->host_depth > 0 &&          \
                (((uint32_t)(ctx)->pc & SPU_LS_MASK) ==                    \
                 ((ctx)->gpr[0]._u32[0] & SPU_LS_MASK))) {                 \
                g_spu_trampoline_fn = 0;                                   \
                break;                                                     \
            }                                                              \
            void (*_tf)(spu_context*) = g_spu_trampoline_fn;    \
            g_spu_trampoline_fn = 0;                            \
            yz_lockstep_tick(ctx);                             \
            spu_task_launch_check((ctx), (void*)_tf);          \
            if ((ctx)->int_enable &&                            \
                (spu_ev_get((ctx)) & (ctx)->event_mask))      \
                _tf = spu_take_interrupt((ctx), _tf);          \
            spu_pchist_tick(ctx);                              \
            (ctx)->pch[(ctx)->pch_n++ & 7u] =                  \
                (uint32_t)((ctx)->pc & SPU_LS_MASK);           \
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

/* Host return through a NON-r0 link register. Sony's SPU compiler links
 * leaf/helper calls through r4/r5/r6/r8/r78 (`brsl $r4, helper` ...
 * `bi $r4`); SPU_RET would publish r0 as the return PC, which is not where
 * the guest is going, and spu_drain_call's return-address check can then
 * never match -- it restarts dispatch from r0's stale value instead. Same
 * host-frame return as SPU_RET, publishing the register actually branched. */
#define SPU_RET_REG(ctx, n) do {                                       (ctx)->pc = (ctx)->gpr[n]._u32[0];                             if ((ctx)->host_depth == 0)                                        g_spu_trampoline_fn = spu_indirect_branch;                 return;                                                    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_CONTEXT_H */
