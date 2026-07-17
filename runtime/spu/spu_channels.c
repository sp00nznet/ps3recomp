/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The SPU decrementer ticks at the PS3 timebase, 79.8 MHz -- the same clock
 * sys_time_get_timebase_frequency reports to the PPU. Titles calibrate real
 * delays against it, so the rate has to be right, not merely non-zero. */
#define SPU_DECREMENTER_HZ  79800000ull

static uint64_t spu_host_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER c;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    /* Split the divide to keep the multiply from overflowing: QPC counters are
     * large enough that (count * 1e9) wraps a u64 within hours of uptime. */
    return (uint64_t)(c.QuadPart / s_freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % s_freq.QuadPart) * 1000000000ull
           / (uint64_t)s_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif
static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;

/* SPU->PPU outbound-mailbox delivery hook. The SPU writing WrOutMbox /
 * WrOutIntrMbox must wake PPU code blocked on the SPURS event queue bound to
 * the SPU thread group (e.g. cellSpursInitialize). lv2_register.c installs a
 * handler that maps spu_group_id -> connected event queue and pushes an event.
 * NULL until installed (plain SPU jobs with no PPU listener stay a no-op). */
void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                            int is_intr, uint32_t value) = 0;

void spu_halt(spu_context* ctx)
{
    (void)ctx;
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; longjmp(s_spu_halt_env, 1); }
}

/* Diagnostic: dump the taskset-policy scheduler's working tables (LS 0x2700..)
 * at func_00000E60 entry, to reverse why it computes "no runnable task". Env
 * YDKJ_E60. Called from the lifted taskset policy. */
void spu_dbg_e60(spu_context* ctx)
{
    static int s_e = -1; if (s_e < 0) s_e = getenv("YDKJ_E60") ? 1 : 0;
    if (!s_e) return;
    static int _d = 0; if (_d++ >= 6) return;
    const uint8_t* L = ctx->ls;
    #define RD(o) (((uint32_t)L[(o)]<<24)|((uint32_t)L[(o)+1]<<16)|((uint32_t)L[(o)+2]<<8)|L[(o)+3])
    fprintf(stderr, "[E60] r20=%08X r24=%08X | run2700=%08X ready2710=%08X pend2720=%08X en2730=%08X sig2740=%08X wait2750=%08X x2770=%08X\n",
            ctx->gpr[20]._u32[0], ctx->gpr[24]._u32[0],
            RD(0x2700), RD(0x2710), RD(0x2720), RD(0x2730), RD(0x2740), RD(0x2750), RD(0x2770));
    #undef RD
    fflush(stderr);
}

/* SPU `stop` / stop-and-signal. A real SPU stop HALTS the core; the previous
 * lifted emission only did `status=...; return;`, which unwound ONE frame and
 * let the caller's service loop keep running -- so a SPURS policy doing
 * stop-and-signal in a loop spun millions of times instead of yielding. Halt the
 * host SPU thread (longjmp to spu_run_with_halt) so the job stops AT the stop,
 * with ctx->status + the outbound mailbox value available for the dispatcher to
 * service. Env YDKJ_STOP_NOHALT restores the old (looping) behavior for A/B. */
void spu_stop(spu_context* ctx)
{
    /* A lifted `stop` sets status and RETURNS to its caller -- for a SPURS
     * policy this is stop-and-signal: it returns up into the kernel/policy
     * service loop, which continues (i.e. the SPU "resumes" past the stop). That
     * resume-by-return behavior is correct; the previous spin was caused by the
     * lack of (a) a PPU listener for the outbound mailbox and (b) mailbox
     * backpressure -- both handled in the channel write path, not here. So by
     * default DO NOT halt. Env YDKJ_STOP_HALT forces a hard halt (longjmp) for
     * A/B experiments (it makes the kernel terminate, which the game restarts). */
    static int s_halt = -1;
    if (s_halt < 0) s_halt = getenv("YDKJ_STOP_HALT") ? 1 : 0;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    if (s_halt) spu_halt(ctx);
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    int halted = 0;
    s_spu_halt_armed = 1;
    if (setjmp(s_spu_halt_env) != 0) halted = 1;   /* came back via longjmp */
    else                             entry(ctx);    /* run the job          */
    s_spu_halt_armed = 0;
    return halted;
}

/* ===========================================================================
 * Per-context MFC engine registry
 * ===========================================================================*/
#define SPU_MAX_CONTEXTS 8

typedef struct {
    spu_context* ctx;
    mfc_engine   mfc;
} spu_mfc_slot;

static spu_mfc_slot s_mfc_slots[SPU_MAX_CONTEXTS];

static mfc_engine* mfc_for(spu_context* ctx)
{
    spu_mfc_slot* free_slot = NULL;
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        if (s_mfc_slots[i].ctx == ctx)
            return &s_mfc_slots[i].mfc;
        if (!free_slot && s_mfc_slots[i].ctx == NULL)
            free_slot = &s_mfc_slots[i];
    }
    if (free_slot) {
        free_slot->ctx = ctx;
        mfc_engine_init(&free_slot->mfc);
        return &free_slot->mfc;
    }
    /* Out of slots: fall back to a shared engine (correct for single-SPU). */
    static mfc_engine fallback;
    static int fallback_init = 0;
    if (!fallback_init) { mfc_engine_init(&fallback); fallback_init = 1; }
    return &fallback;
}

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* Global spinlock guarding all atomic line ops. _InterlockedExchange is a
 * clang-cl/MSVC intrinsic (no runtime library symbol needed). */
#include <intrin.h>
static volatile long g_resv_lock = 0;
static void resv_lock(void)   { while (_InterlockedExchange(&g_resv_lock, 1)) { } }
static void resv_unlock(void) { _InterlockedExchange(&g_resv_lock, 0); }

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_lastea = 0;
        if ((++s_n % 2000000) == 0 || ea != s_lastea) {
          if ((s_n % 2000000) == 0)
            fprintf(stderr, "[atomcnt] %llu atomic ops; last cmd=0x%X ea=0x%08X\n",
                    (unsigned long long)s_n, cmd, ea);
          s_lastea = ea; } } }
    { static int s_at = -1; if (s_at < 0) s_at = getenv("YDKJ_ATOMTRACE") ? 1 : 0;
      if (s_at) { static int _a=0; if (_a++ < 40)
        fprintf(stderr, "[atom] cmd=0x%02X ea=0x%08X (img=%d)\n", cmd, ea, ctx->image_id); } }
    /* cri task (img22) atomic on the taskset: dump the loaded bitset line so we can
     * see if the task reads MY taskset (0x4005E000) with my READY bit, or elsewhere. */
    { static int s_ct=-1; if(s_ct<0) s_ct=getenv("YDKJ_ATOMTRACE")?1:0;
      if(s_ct && ctx->image_id==22 && cmd==0xD0 && mfc_ea_range_committed(ea,16)) {
        static int _c=0; if(_c++<24){
          uint8_t* m=vm_base+ea;
          #define BW(o) (((uint32_t)m[o]<<24)|((uint32_t)m[o+1]<<16)|((uint32_t)m[o+2]<<8)|m[o+3])
          fprintf(stderr,"[cri-atom] GETLLAR ea=0x%08X line[0..0x30]: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X\n",
            ea, BW(0),BW(4),BW(8),BW(0xC), BW(0x10),BW(0x14),BW(0x18),BW(0x1C), BW(0x20),BW(0x24),BW(0x28),BW(0x2C));
          #undef BW
        } } }
    /* YDKJ_CRI_R4: dump the CellSpursTaskset bitsets when the policy atomically
     * touches my taskset (0x0F000000), to watch the task-activation state machine
     * (why task0 isn't selected+first-run). running@0 ready@0x10 pending@0x20
     * enabled@0x30 signalled@0x40 waiting@0x50 (each 16B; word0 = MSB, task0=bit127). */
    { static int s_td = -1; if (s_td < 0) s_td = (getenv("YDKJ_CRI_CHAIN") && getenv("YDKJ_ATOMTRACE")) ? 1 : 0;
      if (s_td && ea >= 0x0F000000u && ea < 0x0F001900u) {
        extern uint8_t* vm_base;
        static int _t=0; if (vm_base && _t++ < 24) {
            uint8_t* t = vm_base + 0x0F000000u;
            #define TW(o) (((uint32_t)t[o]<<24)|((uint32_t)t[o+1]<<16)|((uint32_t)t[o+2]<<8)|t[o+3])
            fprintf(stderr, "[tset] %s run=%08X rdy=%08X pnd=%08X ena=%08X sig=%08X wait=%08X | wid=%08X last=%02X\n",
                    cmd==0xD0?"GET":cmd==0xB4?"PUT":"?", TW(0x00), TW(0x10), TW(0x20), TW(0x30), TW(0x40), TW(0x50), TW(0x74), t[0x73]);
            #undef TW
        }
      } }

    /* Guard atomic line ops against an uncommitted/garbage EA (e.g. a SPURS
     * policy computing a lock-line address from an incomplete instance context).
     * Same rationale as the DMA EA guard: a bad guest atomic must not segfault
     * the host. GETLLAR returns a zeroed line (no reservation); PUTLLC fails. */
    if (!mfc_ea_range_committed(ea, MFC_ATOMIC_LINE)) {
        static int s_w = 0;
        if (s_w++ < 16)
            fprintf(stderr, "[spu-atomic] cmd=0x%X ea=0x%08X uncommitted -- skipped\n", cmd, ea);
        if (cmd == MFC_GETLLAR_CMD) {
            memset(ls, 0, MFC_ATOMIC_LINE);
            ctx->resv_ea = ea; ctx->resv_valid = 0; ctx->atomic_stat = 0;
        } else {
            ctx->atomic_stat = 1;   /* PUTLLC failure (line "moved") */
        }
        return 1;
    }

    switch (cmd) {
    case MFC_GETLLAR_CMD:
        resv_lock();
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        memcpy(ctx->resv_line, mem, MFC_ATOMIC_LINE);  /* snapshot for compare */
        ctx->resv_ea = ea; ctx->resv_valid = 1; ctx->atomic_stat = 0;
        resv_unlock();
        return 1;

    case MFC_PUTLLC_CMD:
        resv_lock();
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            memcmp(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0) {
            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
        }
        ctx->resv_valid = 0;                           /* reservation consumed */
        resv_unlock();
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        resv_lock();
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        ctx->resv_valid = 0; ctx->atomic_stat = 0;
        resv_unlock();
        return 1;

    default:
        return 0;
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        if (channel == MFC_Cmd && spu_mfc_atomic(ctx, v))
            return;
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        spu_channel_write(&ctx->ch_out_mbox, v);
        { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] OUT  grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 0, v);
        break;
    case SPU_WrOutIntrMbox:
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] INTR grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;
                             ctx->dec_base_ns = spu_host_ns();              break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break; /* WrEventMask */
    case SPU_WrEventAck:     ctx->event_status &= ~v;                       break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    uint32_t v = 0;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_c[10] = {0}; static uint64_t s_tot = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdSigNotify1)?1:(channel==SPU_RdSigNotify2)?2:
                (channel==SPU_RdDec)?3:(channel==SPU_RdEventStat)?4:(channel==SPU_RdEventMask)?5:
                (channel==MFC_RdTagStat)?6:(channel==MFC_RdAtomicStat)?7:(channel==SPU_RdMachStat)?8:9;
        s_c[b]++;
        if ((++s_tot % 2000000) == 0)
          fprintf(stderr, "[rdch] InMbox=%llu Sig1=%llu Sig2=%llu Dec=%llu EvStat=%llu EvMask=%llu TagStat=%llu AtomStat=%llu MachStat=%llu other=%llu\n",
            (unsigned long long)s_c[0],(unsigned long long)s_c[1],(unsigned long long)s_c[2],(unsigned long long)s_c[3],
            (unsigned long long)s_c[4],(unsigned long long)s_c[5],(unsigned long long)s_c[6],(unsigned long long)s_c[7],
            (unsigned long long)s_c[8],(unsigned long long)s_c[9]); } }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:      v = spu_channel_read(&ctx->ch_in_mbox);     break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    /* The decrementer must actually DECREMENT. Returning the latched WrDec
     * value verbatim makes every `rdch $ch8` deadline poll spin forever: the
     * elapsed-time term is always zero, so the deadline never passes. LBP's
     * wwsjob SPURS policy module hangs on exactly that at LS 0x2D68:
     *
     *     2d6c:  lqa   $2, 0x1530     ; deadline
     *     2d74:  rdch  $5, $ch8       ; now  <- always read 0
     *     2d78:  sf    $6, $2, $5     ; now - deadline
     *     2d80:  cgti  $5, $6, 0
     *     2d88:  binz  $2, $0         ; leave once it goes positive
     *
     * It never leaves, never reaches its first DMA, and blocks the workload.
     * Wraparound on subtract is deliberate -- it is what hardware does, and the
     * `cgti > 0` test above is written to tolerate it. */
    case SPU_RdDec: {
        /* Never armed by a WrDec: stamp the base on first read rather than
         * measuring from epoch 0, which would subtract the host's entire
         * uptime and hand back a wild value to code that has every right to
         * read the decrementer before writing it. */
        if (!ctx->dec_base_ns) ctx->dec_base_ns = spu_host_ns();
        uint64_t elapsed = spu_host_ns() - ctx->dec_base_ns;
        uint32_t ticks   = (uint32_t)((elapsed * SPU_DECREMENTER_HZ)
                                      / 1000000000ull);
        v = ctx->decrementer - ticks;
        break;
    }
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   v = ctx->event_status;                      break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_cnt[8] = {0}; static uint64_t s_total = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdEventStat)?1:(channel==SPU_RdSigNotify1)?2:
                (channel==SPU_RdSigNotify2)?3:(channel==MFC_RdTagStat)?4:(channel==SPU_WrOutMbox)?5:
                (channel==SPU_WrOutIntrMbox)?6:7;
        s_cnt[b]++;
        if ((++s_total % 2000000) == 0)
          fprintf(stderr, "[pollcnt] InMbox=%llu EvStat=%llu Sig1=%llu Sig2=%llu TagStat=%llu OutMbox=%llu OutIntr=%llu other=%llu (ch last=%u)\n",
                  (unsigned long long)s_cnt[0],(unsigned long long)s_cnt[1],(unsigned long long)s_cnt[2],
                  (unsigned long long)s_cnt[3],(unsigned long long)s_cnt[4],(unsigned long long)s_cnt[5],
                  (unsigned long long)s_cnt[6],(unsigned long long)s_cnt[7], channel); } }
    switch (channel) {
    case SPU_RdInMbox:       return ctx->ch_in_mbox.count;                 /* readable */
    case SPU_WrOutMbox:      return SPU_MBOX_DEPTH - ctx->ch_out_mbox.count; /* free slots */
    case SPU_WrOutIntrMbox:  return SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count;
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

#define SPU_FN_REGISTRY_MAX 65536
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count < SPU_FN_REGISTRY_MAX) {
        s_registry[s_registry_count].addr = addr;
        s_registry[s_registry_count].fn = fn;
        s_registry[s_registry_count].image_id = s_reg_image;
        s_registry_count++;
    }
}

static spu_fn spu_lookup(uint32_t addr, int image_id)
{
    /* Linear scan is fine for the small per-image tables. Match the context's
     * active image; image_id 0 (context or entry) matches any, for back-compat
     * with single-image contexts. */
    for (uint32_t i = 0; i < s_registry_count; i++)
        if (s_registry[i].addr == addr &&
            (image_id == 0 || s_registry[i].image_id == 0 ||
             s_registry[i].image_id == image_id))
            return s_registry[i].fn;
    return NULL;
}

/* HLE of the taskset Policy Module's task-syscall entry (LS 0xA70). A SPURS task
 * (e.g. the cri_mpv task, image 22) reads syscallAddr from its SpursTasksetContext
 * (LS 0x27C4) and branches to it to perform a task syscall (EXIT/YIELD/WAIT/POLL).
 * The real kernel has the PM code resident at 0xA70; we don't, so we plant 0xA70 as
 * syscallAddr (in the cri dispatch) and INTERCEPT a branch to it here to HLE the
 * syscall. num = r3&0xF (0x10 bit = the "2" variant), args in r4. Adopted from the
 * JonathanDC64/ps3recomp fork (aaea4158) which uses this to run SPURS tasks clean. */
#define YDKJ_TASKSET_PM_SYSCALL_ADDR 0xA70u
static void spu_spurs_taskset_syscall(spu_context* ctx)
{
    uint32_t raw = ctx->gpr[3]._u32[0];
    uint32_t num = raw & 0x0F;
    { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[spu] SPURS taskset syscall num=%u (raw=0x%X args=0x%08X) image=%d link/r0=0x%05X\n",
                num, raw, ctx->gpr[4]._u32[0], ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    /* NOTE (YDKJ cri_mpv): the cri task's BOOTSTRAP (func_00003040) calls the
     * task-API syscall and EXPECTS IT TO RETURN, then branches to the real task
     * entry (0x3050). Halting on num=0 here kills the task at bootstrap before it
     * runs. So for image 22 we DON'T halt on num=0 -- we return so the bootstrap
     * continues to the decode entry. (A genuine end-of-task EXIT would re-enter and
     * spin; if that happens, gate a real halt after the task has done work.)
     * For non-cri images keep the fork's EXIT=halt semantics. Env YDKJ_CRI_EXIT_HALT
     * forces the old halt behaviour for comparison. */
    if (num == 0 && (ctx->image_id != 22 || getenv("YDKJ_CRI_EXIT_HALT"))) {
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);          /* longjmp out to spu_run_with_halt; post-run writes exit code */
        return;
    }
    /* WAIT_SIGNAL(2): REAL semantics (RPCS3 spursTasksetProcessSyscall) --
     * consume the task's bit in the guest taskset's `signalled` bitset, or
     * SLEEP this task's host thread until _cellSpursSendSignal /
     * cellSpursEventFlagSet delivers one (the FMOD mixer's flag-A wait path:
     * the task registers its wait slot in the flag struct with atomics, then
     * syscalls WAIT_SIGNAL; the PPU-side Set satisfies the slot and signals).
     * Returning "success" here without waiting made the task spin on empty
     * work state forever -- the LBP boot deadlock. taskset/taskId come from
     * the SpursTasksetContext this runtime planted at LS 0x2700. */
    if (num == 2) {
        uint32_t ts  = ((uint32_t)ctx->ls[0x27BC] << 24) | ((uint32_t)ctx->ls[0x27BD] << 16) |
                       ((uint32_t)ctx->ls[0x27BE] << 8)  |  (uint32_t)ctx->ls[0x27BF];
        uint32_t tid = ((uint32_t)ctx->ls[0x27D4] << 24) | ((uint32_t)ctx->ls[0x27D5] << 16) |
                       ((uint32_t)ctx->ls[0x27D6] << 8)  |  (uint32_t)ctx->ls[0x27D7];
        if (ts) {
            extern int spu_taskset_wait_signal(uint32_t, uint32_t);
            spu_taskset_wait_signal(ts, tid);
        }
        ctx->gpr[3]._u32[0] = 0;
        return;
    }
    /* EXIT(0, cri bootstrap)/YIELD(1)/POLL(3)/RECV_WKL_FLAG(4):
     * report success and resume (return -> lifted caller continues at its link). */
    ctx->gpr[3]._u32[0] = 0;
}

void spu_indirect_branch(spu_context* ctx)
{
    /* Real SPU bi/bisl mask the target to the 256 KB local store; the high bits
     * of a computed pointer (e.g. a packed handle like 0x7a028803) are ignored.
     * Without this, any indirect branch through such a value fails the lookup
     * and falls into branch-to-0. All lifted funcs live below SPU_LS_SIZE, so
     * masking is a no-op for already-valid targets. */
    ctx->pc &= SPU_LS_MASK;
    /* SPURS kernel services (policy-module runs only): the HLE kernel plants
     * these two reserved addresses as exitToKernelAddr / selectWorkloadAddr in
     * the SpursKernelContext (spurs_policy.c). */
    if (ctx->policy_mode) {
        extern volatile unsigned g_spurs_pm_polls, g_spurs_pm_exited;
        if (ctx->pc == SPURS_PM_EXIT_TO_KERNEL_LS) {
            /* Module exit: the workload returned to the kernel (drained/yield). */
            g_spurs_pm_exited = 1;
            fprintf(stderr, "[spurs-pm] exit-to-kernel (r3=0x%08X polls=%u)\n",
                    ctx->gpr[3]._u32[0], g_spurs_pm_polls);
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (ctx->pc == SPURS_PM_SELECT_WORKLOAD_LS) {
            /* cellSpursModulePoll: report "no contention — keep running".
             * (One virtual SPU per workload here, so nothing ever preempts.) */
            unsigned n = ++g_spurs_pm_polls;
            if (n <= 4 || (n % 4096) == 0)
                fprintf(stderr, "[spurs-pm] poll #%u (r3=0x%08X) -> continue\n",
                        n, ctx->gpr[3]._u32[0]);
            ctx->gpr[3] = spu_make_preferred_u32(0);
            return;
        }
    }
    /* Taskset PM task-syscall entry (LS 0xA70): HLE it instead of branching into
     * (absent) PM code. Fires for the cri task (image 22) AND any generic taskset
     * task whose SpursTasksetContext we planted -- detected by the syscallAddr
     * sentinel at LS 0x27C4 (== 0xA70), which only spurs_pm_build_context writes.
     * Without generalizing this, an LBP FMOD task that reaches its EXIT/YIELD
     * syscall would branch into empty LS 0xA70 and halt as "branch-to-0" instead
     * of cleanly exiting. */
    if (ctx->pc == YDKJ_TASKSET_PM_SYSCALL_ADDR) {
        uint32_t sc = ((uint32_t)ctx->ls[0x27C4] << 24) | ((uint32_t)ctx->ls[0x27C5] << 16)
                    | ((uint32_t)ctx->ls[0x27C6] << 8)  | ctx->ls[0x27C7];
        if (ctx->image_id == 22 || sc == YDKJ_TASKSET_PM_SYSCALL_ADDR) {
            spu_spurs_taskset_syscall(ctx); return;
        }
    }
    /* YDKJ_CRI_R4: the taskset policy entry (LS 0xA00, image 23) writes r4 into
     * SpursTasksetContext.taskset @LS 0x27B8 (per RPCS3 cellSpursSpu.cpp). Our
     * kernel->policy handoff doesn't convey the taskset EA, so the policy DMAs
     * the taskset from garbage -> waiting!=0 -> wrong resume path -> savedContextLr=0.
     * Inject r4 = taskset EA (0x0F000000) at the policy entry dispatch (this is the
     * exact point before the entry reads r4, after the kernel's arg setup). */
    if (ctx->image_id == 23 && !getenv("YDKJ_NO_CRI_R4")) {
        static int s_r4 = -1; if (s_r4 < 0) s_r4 = getenv("YDKJ_CRI_CHAIN") ? 1 : 0;
        if (s_r4) {
            /* Force ctxt->taskset @LS 0x27B8 = the REAL game taskset EA on every
             * image-23 branch, so the policy's atomic reads + context-EA computation
             * use the actual taskset (the r4 handoff sets it to garbage 0x0000FFFF via
             * a path we can't intercept). Was hardcoded 0x0F000000, which mismatched
             * the game's real taskset (0x45F1B000) -> policy DMA'd garbage. */
            extern uint32_t g_ydkj_real_taskset_ea;
            uint32_t ts = g_ydkj_real_taskset_ea ? g_ydkj_real_taskset_ea : 0x0F000000u;
            ctx->ls[0x27B8]=0x00; ctx->ls[0x27B9]=0x00; ctx->ls[0x27BA]=0x00; ctx->ls[0x27BB]=0x00;
            ctx->ls[0x27BC]=(uint8_t)(ts>>24); ctx->ls[0x27BD]=(uint8_t)(ts>>16); ctx->ls[0x27BE]=(uint8_t)(ts>>8); ctx->ls[0x27BF]=(uint8_t)ts;
            if (ctx->pc == 0xA00u) { static int _n=0; if (_n++ < 4)
                fprintf(stderr, "[cri-r4] policy entry pc=0xA00: forced ctxt->taskset LS[0x27B8]=0x0F000000\n"); }
        }
    }
    { static int s_ib = -1; if (s_ib < 0) s_ib = getenv("YDKJ_IBTRACE") ? 1 : 0;
      if (s_ib && ctx->image_id == 23) { static int _i = 0; if (_i++ < 60)
        fprintf(stderr, "[ib23] target=0x%05X lr=0x%05X\n",
                ctx->pc, ctx->gpr[0]._u32[0] & 0x3FFFF); } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_last = 0; static uint64_t s_run = 0;
        if (ctx->pc == s_last) s_run++; else { s_last = ctx->pc; s_run = 1; }
        if ((++s_n % 2000000) == 0)
          fprintf(stderr, "[ibranch] %llu indirect branches; current target=0x%05X run=%llu\n",
                  (unsigned long long)s_n, ctx->pc, (unsigned long long)s_run); } }
    /* Policy-entry trace: the SPURS policy at LS 0xA00 branches on
     * r8 = word at LS[r6] (must be 32 for the path that sets the dispatch ptr
     * LS[0x780]). Log r3..r6 + the word the kernel handed it, to see why the
     * wrong branch is taken. Env YDKJ_POLTRACE. */
    if (ctx->pc == 0xA00u) {
        static int64_t pt=-2; if (pt==-2){ const char* e=getenv("YDKJ_POLTRACE"); pt=e?1:0; }
        if (pt) { static int _p=0; if (_p++ < 8) {
            /* re-lifted policy entry uses r80 (kernel-set context base): r44=LS[r80+0xC0] */
            uint32_t r80=ctx->gpr[80]._u32[0] & SPU_LS_MASK;
            const uint8_t* q = ctx->ls + ((r80+0xC0)&SPU_LS_MASK);
            uint32_t ctxw = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
            fprintf(stderr, "[POLTRACE] policy@0xA00 r3=%08X r4=%08X r80=%08X  LS[r80+0xC0]=%08X\n",
                ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], r80, ctxw);
            fflush(stderr);
        } }
    }
    spu_fn fn = spu_lookup(ctx->pc, ctx->image_id);
    if (fn) {
        fn(ctx);
        return;
    }
    /* Cap the unresolved-branch log PER IMAGE: a global cap let one noisy
     * image (the FMOD mixer's overlay calls) exhaust it and silently hide
     * every other image's misses -- LBP's loading jobs skipped their command
     * handlers for a whole session without a single log line. */
    { enum { BT0_MAX_IMG = 64, BT0_PER_IMG = 12 };
      static int _bt0[BT0_MAX_IMG];
      unsigned img = (ctx->image_id >= 0 && ctx->image_id < BT0_MAX_IMG)
                     ? (unsigned)ctx->image_id : 0;
      if (_bt0[img]++ < BT0_PER_IMG)
        fprintf(stderr, "[SPU] BRANCH-TO-0 unresolved pc=0x%05X image=%d lr=0x%05X\n",
                ctx->pc, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    { static int _n=0; if (_n++ < 2) {
        fprintf(stderr, "[SPU] branch-to-0 lr=0x%05X r1=0x%05X\n",
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0] & SPU_LS_MASK);
#ifdef _WIN32
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
#endif
        /* State-diff oracle: dump full LS + all GPRs at the branch-to-0 so it can
         * be compared byte-for-byte against the RPCS3 savestate LS of the same
         * (cri_mpv) task. Path from YDKJ_SPU_LSDUMP, else ./recomp_spu_ls.bin. */
        const char* dp = getenv("YDKJ_SPU_LSDUMP");
        if (!dp || !*dp) dp = "recomp_spu_ls.bin";
        FILE* lf = fopen(dp, "wb");
        if (lf) { fwrite(ctx->ls, 1, SPU_LS_SIZE, lf); fclose(lf);
                  fprintf(stderr, "[SPU] dumped 256KB LS -> %s\n", dp); }
        fprintf(stderr, "[SPU] image_id=%d  GPR dump (r0..r127, hi64:lo64 of each quadword, preferred slot = _u32[0]):\n", ctx->image_id);
        for (int g = 0; g < 128; g++) {
            fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                    ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                    ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
            if ((g & 1) == 1) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        /* echo the dispatch chain values the way func_00026DE0 computes them */
        { uint32_t bec0 = ctx->ls[0xBEC0]<<24 | ctx->ls[0xBEC1]<<16 | ctx->ls[0xBEC2]<<8 | ctx->ls[0xBEC3];
          fprintf(stderr, "[SPU] LS[0xBEC0].w0=0x%08X  LS[0x2d4e0:16]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
            bec0,
            ctx->ls[0x2d4e0],ctx->ls[0x2d4e1],ctx->ls[0x2d4e2],ctx->ls[0x2d4e3],
            ctx->ls[0x2d4e4],ctx->ls[0x2d4e5],ctx->ls[0x2d4e6],ctx->ls[0x2d4e7],
            ctx->ls[0x2d4e8],ctx->ls[0x2d4e9],ctx->ls[0x2d4ea],ctx->ls[0x2d4eb],
            ctx->ls[0x2d4ec],ctx->ls[0x2d4ed],ctx->ls[0x2d4ee],ctx->ls[0x2d4ef]); }
    } }
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

/* Bounded by design: a --trace lift emits one call PER INSTRUCTION, so an image
 * that spins (a persistent SPURS policy module polling its job queue never
 * returns) would otherwise write stderr until the disk fills. The cap keeps the
 * useful part -- the entry path plus enough revolutions to show what the loop
 * tests -- and the tail IS the loop. SPU_TRACE_MAX=0 restores unbounded.
 * SPU_TRACE_FILE redirects off stderr so the trace does not interleave with the
 * boot log. */
void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    static long long s_left = -1;
    if (s_left < 0) {
        const char* p = getenv("SPU_TRACE_FILE");
        if (p && *p && !s_trace_fp) spu_trace_init(p);
        const char* m = getenv("SPU_TRACE_MAX");
        s_left = (m && *m) ? atoll(m) : 200000;
        if (s_left == 0) s_left = -2;          /* -2 = unbounded */
    }
    if (!s_trace_fp) s_trace_fp = stderr;
    if (s_left == 0) return;
    if (s_left > 0 && --s_left == 0) {
        fprintf(s_trace_fp, "-- SPU_TRACE_MAX reached; trace stopped --\n");
        fflush(s_trace_fp);
        return;
    }
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
