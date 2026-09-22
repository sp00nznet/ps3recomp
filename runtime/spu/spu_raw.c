/*
 * ps3recomp - raw SPU: the 0xE0000000 MMIO window and the sys_raw_spu_* syscalls.
 *
 * See spu_raw.h for the memory map. The short version: lv2 hands the process a
 * physical SPU, maps its local store and problem-state registers into the address
 * space, and then gets out of the way. Everything after sys_raw_spu_create is
 * ordinary loads and stores by the PPU:
 *
 *      *(u32*)(base + 0x44034) = start_pc;      // SPU_NPC
 *      *(u32*)(base + 0x4401C) = 1;             // SPU_RunCntl  -> the SPU starts
 *      while (!(*(u32*)(base + 0x44014) & 0xFF))  ;  // poll SPU_MBox_Status
 *      u32 reply = *(u32*)(base + 0x44004);     // SPU_Out_MBox
 *
 * which is why this cannot live in the SPURS workload layer: there is no dispatch
 * call to intercept. The only interception points are the loads and stores
 * themselves, so vm_write32/vm_read32 call in here for addresses in the window's
 * PROBLEM STATE (local store stays on the plain memory fast path -- it is the bulk
 * of the traffic and needs no side effects).
 *
 * Which lifted image runs: the guest imports its SPU ELF with
 * _sys_spu_image_import before starting the SPU, so spu_raw_note_image()
 * fingerprints that ELF the same way the offline extractor does (FNV-1a-64 over
 * the exact file bytes) and looks it up in the shared workload registry. The port
 * registers its lifted entries there as usual -- see a title's src/spu_images.c.
 * That keeps raw SPUs on exactly the same registration mechanism as SPURS jobs
 * instead of inventing a second one.
 */

#include "spu_raw.h"
#include "spu_context.h"
#include "spu_workload.h"
#include "spu_lifted_job.h"
#include "../syscalls/lv2_syscall_table.h"
#include "../../include/ps3emu/error_codes.h"
#include "ps3emu/nid.h"      /* ps3_compute_nid */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>      /* nanosleep, in the POSIX half of spu_intr.inc */

#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern int g_spu_force_ch_block;              /* spu_channels.c */
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
/* For attributing an MMIO store to the guest code that made it. */
#include "../ppu/ppu_context.h"
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern void ps3_hle_register_ctx(uint32_t nid, const char* name,
                                 void (*fn)(ppu_context*));

/* Run-control values written to SPU_RunCntl. */
#define RUNCNTL_STOP  0x0u
#define RUNCNTL_RUN   0x1u

/* SPU_MBox_Status field layout: [7:0] out count, [15:8] in free slots,
 * [23:16] out-interrupt count.
 *
 * Hardware's PPU->SPU mailbox is four deep and the guest checks the free-slot
 * field before every write. This used to advertise ONE, because spu_channel
 * held one value -- claiming four would have invited the PPU to write three
 * words the channel then dropped. spu_channel now holds SPU_CHANNEL_CAP, so
 * report the real depth: a title sending a multi-word work descriptor (The
 * Orange Box sends CB.SPU two words) otherwise has all but one silently
 * discarded, and the SPU blocks forever on a message it half received. */
/* Defined in spu_intr.inc, included below; used by the mailbox hook above it. */
static void ps3_intr_raise(uint32_t tag);

#define SPU_RAW_IN_MBOX_DEPTH  SPU_CHANNEL_CAP
#define MBOX_STATUS(out_n, in_free, intr_n) \
    (((uint32_t)(out_n) & 0xFF) | (((uint32_t)(in_free) & 0xFF) << 8) | \
     (((uint32_t)(intr_n) & 0xFF) << 16))

typedef struct raw_spu {
    int          used;
    uint32_t     base;          /* guest EA of the 1 MB window */
    spu_context* ctx;
    spu_lifted_entry_fn entry;  /* lifted code for the image loaded into this SPU */
    uint32_t     intrtag;
    uint32_t     int_mask;
    uint32_t     int_stat;
    volatile long started;      /* the host thread is live */
#ifdef _WIN32
    HANDLE       thread;
#endif
} raw_spu;

static raw_spu s_spu[SPU_RAW_COUNT];

static int dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SPU_RAW_DBG") ? 1 : 0;
    return v;
}

/* ---------------------------------------------------------------------------
 * Register access.
 *
 * The registers ARE guest memory, so they are read and written here directly
 * rather than through vm_read32/vm_write32 -- those are the functions that call
 * into this file, and routing back through them would recurse.
 * -----------------------------------------------------------------------*/
static uint32_t be32_load(uint32_t ea)
{
    uint32_t b;
    memcpy(&b, vm_base + ea, 4);
    return (b >> 24) | ((b >> 8) & 0xFF00u) | ((b << 8) & 0xFF0000u) | (b << 24);
}

static void be32_store(uint32_t ea, uint32_t v)
{
    uint32_t b = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    memcpy(vm_base + ea, &b, 4);
}

static raw_spu* spu_for(uint32_t ea, uint32_t* off_out)
{
    uint32_t off = ea - SPU_RAW_BASE;
    uint32_t n   = off / SPU_RAW_STRIDE;
    if (n >= SPU_RAW_COUNT) return NULL;
    *off_out = off - n * SPU_RAW_STRIDE;
    return &s_spu[n];
}

/* Publish the SPU-visible state into the window the PPU polls. */
static void publish(raw_spu* s)
{
    spu_context* c = s->ctx;
    if (!c) return;
    be32_store(s->base + SPU_RAW_STATUS, c->status);
    be32_store(s->base + SPU_RAW_MBOX_STATUS,
               MBOX_STATUS(c->ch_out_mbox.count,
                           SPU_RAW_IN_MBOX_DEPTH - c->ch_in_mbox.count,
                           c->ch_out_intr_mbox.count));
    if (c->ch_out_mbox.count)
        be32_store(s->base + SPU_RAW_OUT_MBOX, c->ch_out_mbox.value);
}

/* ---------------------------------------------------------------------------
 * Outbound mailbox: the SPU wrote, the PPU is polling.
 *
 * spu_channels.c already calls g_spu_out_mbox_hook on every WrOutMbox /
 * WrOutIntrMbox, which is the only place a raw SPU's write can be seen without
 * touching the channel code. Chain whatever was there so a port that installs its
 * own delivery hook keeps working.
 * -----------------------------------------------------------------------*/
extern void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                                   int is_intr, uint32_t value);
static void (*s_prev_out_hook)(uint32_t, uint32_t, int, uint32_t);

static void raw_out_mbox_hook(uint32_t group, uint32_t spu_id, int is_intr, uint32_t v)
{
    if (spu_id < SPU_RAW_COUNT && s_spu[spu_id].used && s_spu[spu_id].ctx) {
        raw_spu* s = &s_spu[spu_id];
        /* Class-2 status bits, per CBEA: 0x1 is the SPU *interrupt* mailbox
         * (SPU_WrOutIntrMbox) and 0x10 is the plain outbound mailbox threshold.
         * ps1_netemu unmasks 0x3 -- interrupt mailbox plus stop-and-signal -- so
         * a plain WrOutMbox deliberately does NOT interrupt, and briefly mapping
         * it onto 0x1 made the handler run and then call read_puint_mb for a
         * message that was never in the privileged mailbox. */
        be32_store(s->base + SPU_RAW_OUT_MBOX, v);
        s->int_stat |= is_intr ? 0x1u : 0x10u;
        if (s->int_stat & s->int_mask) ps3_intr_raise(s->intrtag);
        publish(s);
        if (dbg())
            fprintf(stderr, "[spu-raw] spu%u out%s mbox = 0x%08X\n",
                    spu_id, is_intr ? "-intr" : "", v);
        return;
    }
    if (s_prev_out_hook) s_prev_out_hook(group, spu_id, is_intr, v);
}

/* ---------------------------------------------------------------------------
 * The image the guest most recently imported.
 *
 * Called from the _sys_spu_image_import handler. The fingerprint is FNV-1a-64
 * over the exact ELF file bytes -- identical to what tools/extract_spu_images.py
 * hashes and what a port's registration passes to spu_workload_register_img -- so
 * a raw SPU resolves through the same registry as everything else. The file
 * length is derived from the section-header table end, which is where an SPU ELF
 * emitted by the SDK ends.
 * -----------------------------------------------------------------------*/
static spu_lifted_entry_fn s_pending_entry;
static int                 s_pending_image_id;
static uint32_t            s_pending_elf_entry;

void spu_raw_note_image(uint32_t src_ea, uint32_t entry)
{
    if (!vm_base) return;
    const uint8_t* e = vm_base + src_ea;
    uint32_t shoff = ((uint32_t)e[0x20] << 24) | ((uint32_t)e[0x21] << 16) |
                     ((uint32_t)e[0x22] << 8)  |  (uint32_t)e[0x23];
    uint32_t shentsz = ((uint32_t)e[0x2E] << 8) | e[0x2F];
    uint32_t shnum   = ((uint32_t)e[0x30] << 8) | e[0x31];
    uint32_t len = shoff + shentsz * shnum;
    if (!len || len > 0x100000u) return;          /* not a plausible SPU ELF */

    /* SPU_RAW_DUMP=<path>: write the image exactly as the guest presents it, so a
     * fingerprint that does not match the registered one can be diffed against the
     * file the offline extractor produced. */
    { const char* dp = getenv("SPU_RAW_DUMP");
      if (dp) { FILE* f = fopen(dp, "wb"); if (f) { fwrite(e, 1, len, f); fclose(f);
                fprintf(stderr, "[spu-raw] dumped %u bytes to %s%c", len, dp, 10); } } }
    uint64_t fp = spu_workload_fingerprint(e, len);
    int image_id = 0;
    spu_lifted_entry_fn fn = spu_workload_find_img(fp, &image_id);
    s_pending_entry     = fn;
    s_pending_image_id  = image_id;
    s_pending_elf_entry = entry;

    fprintf(stderr, "[spu-raw] image_import src=0x%08X len=%u entry=0x%05X "
                    "fp=0x%016llX -> %s\n",
            src_ea, len, entry, (unsigned long long)fp,
            fn ? "lifted entry registered" : "NO LIFTED IMAGE (SPU will not run)");
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * The SPU host thread
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static DWORD WINAPI raw_spu_thread(LPVOID arg)
{
    raw_spu* s = (raw_spu*)arg;
    spu_context* c = s->ctx;

    c->status = SPU_STATUS_RUNNING;
    publish(s);

    int halted = spu_run_with_halt(s->entry, c);

    /* A raw SPU that returns has executed `stop`. Report it the way hardware
     * does -- run bit clear, stop bit set, stop code in the high half -- so the
     * PPU's status poll sees a real termination instead of a stuck 'running'. */
    c->status = SPU_STATUS_STOPPED_BY_STOP | (c->stop_code << 16);
    /* Stop-and-signal is an INTERRUPT, not just a status change. This is how a
     * raw SPU asks the PPU for something: `stop <code>`, the PPU takes the
     * class-2 interrupt (status bit 0x2), reads the code, services it and
     * restarts the SPU. ps1_netemu unmasks exactly this bit, and without it the
     * GPU core stopping with code 0x2000 looked like a clean exit that nobody
     * was ever told about -- so nobody restarted it. */
    s->int_stat |= 0x2u;
    if (s->int_stat & s->int_mask) ps3_intr_raise(s->intrtag);
    publish(s);
    InterlockedExchange(&s->started, 0);

    fprintf(stderr, "[spu-raw] spu%u stopped: halted=%d pc=0x%05X lr=0x%05X "
                    "steps=%llu stop_code=0x%X outmbox(n=%u v=0x%08X) "
                    "outintr(n=%u v=0x%08X) inmbox(n=%u)\n",
            (unsigned)(s - s_spu), halted, (unsigned)(c->pc & SPU_LS_MASK),
            (unsigned)(c->gpr[0]._u32[0] & SPU_LS_MASK),
            (unsigned long long)c->steps, c->stop_code,
            (unsigned)c->ch_out_mbox.count, c->ch_out_mbox.value,
            (unsigned)c->ch_out_intr_mbox.count, c->ch_out_intr_mbox.value,
            (unsigned)c->ch_in_mbox.count);
    fflush(stderr);
    return 0;
}
#endif

static void raw_spu_start(raw_spu* s)
{
    uint32_t n = (uint32_t)(s - s_spu);
    if (!s_pending_entry) {
        fprintf(stderr, "[spu-raw] spu%u run requested but no lifted image is "
                        "registered -- the SPU cannot run. Lift the image and "
                        "register it (see a port's src/spu_images.c).\n", n);
        fflush(stderr);
        return;
    }
#ifdef _WIN32
    if (InterlockedCompareExchange(&s->started, 1, 0) != 0) return;  /* already up */
#endif

    if (!s->ctx) {
        s->ctx = (spu_context*)malloc(sizeof(spu_context));
        if (!s->ctx) return;
    }
    spu_context_init(s->ctx, n);

    /* THE point of this whole file: local store is the guest window, not a copy.
     * The PPU keeps writing command buffers into it while the SPU runs. */
    s->ctx->ls       = vm_base + s->base;
    s->ctx->image_id = s_pending_image_id;
    s->entry         = s_pending_entry;
    s->ctx->gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;      /* SPU ABI stack top */

    uint32_t npc = be32_load(s->base + SPU_RAW_NPC) & ~3u;
    s->ctx->pc = npc ? npc : s_pending_elf_entry;

    g_spu_force_ch_block = 1;   /* an empty RdInMbox must park, not fabricate 0 */

    /* Local store content at the start of the run. An all-zero LS means nobody
     * loaded the image -- the SPU would branch straight into zeros and halt at
     * pc 0, which is indistinguishable from a lifting bug unless it is reported
     * here. The word at the entry is the first instruction it will execute. */
    { const uint8_t* l = s->ctx->ls;
      uint32_t nz = 0, pc = s->ctx->pc & SPU_LS_MASK;
      for (uint32_t i = 0; i < SPU_LS_SIZE; i += 64) if (l[i]) nz++;
      fprintf(stderr, "[spu-raw] spu%u START pc=0x%05X ls=guest:0x%08X image=%d  "
                      "LS[0..7]=%02X%02X%02X%02X%02X%02X%02X%02X  "
                      "entry=%02X%02X%02X%02X  nonzero lines=%u/4096\n",
              n, pc, s->base, s->ctx->image_id,
              l[0],l[1],l[2],l[3],l[4],l[5],l[6],l[7],
              l[pc],l[pc+1],l[pc+2],l[pc+3], nz); }
    fflush(stderr);

#ifdef _WIN32
    s->thread = CreateThread(NULL, 1u << 20, raw_spu_thread, s, 0, NULL);
    if (!s->thread) InterlockedExchange(&s->started, 0);
#endif
}

/* ---------------------------------------------------------------------------
 * MMIO
 * -----------------------------------------------------------------------*/
void spu_raw_reg_store(uint32_t ea, uint32_t val, int width)
{
    uint32_t off;
    raw_spu* s = spu_for(ea, &off);
    if (!s) return;

    if (dbg())
        fprintf(stderr, "[spu-raw] W spu%u +0x%05X = 0x%08X (w%d) from lr=0x%08X\n",
                (unsigned)(s - s_spu), off, val, width,
                g_active_ctx ? (uint32_t)g_active_ctx->lr : 0);

    switch (off) {
    case SPU_RAW_RUNCNTL:
        if (val == RUNCNTL_RUN) raw_spu_start(s);
        else if (s->ctx)        s->ctx->status = SPU_STATUS_STOPPED;
        break;

    case SPU_RAW_IN_MBOX:
        if (s->ctx) {
            /* SPU_MBOX_CHAIN=<n>: the guest call chain behind each of the first
             * n inbound mailbox writes, per SPU.
             *
             * A title that dispatches SPU work by mailbox and then STOPS has
             * its answer above the dispatcher, not in it -- the dispatcher is a
             * one-shot and its caller is what gave up. The back-chain LR slots
             * are 0 under the fragment model, so scan the stack for words in
             * the lifted code range, the same idiom [LWM-CONVOY] uses. Compare
             * the chain of the last write against an earlier one: the frame
             * that is present early and missing late is the one that stopped
             * asking. */
            { static int cap = -1; static int seen[SPU_RAW_COUNT];
              if (cap < 0) { const char* e = getenv("SPU_MBOX_CHAIN");
                             cap = e ? atoi(e) : 0; }
              uint32_t si = (uint32_t)(s - s_spu);
              if (cap && si < SPU_RAW_COUNT && seen[si] < cap && g_active_ctx) {
                  seen[si]++;
                  uint32_t sp = (uint32_t)g_active_ctx->gpr[1];
                  char b[1200];
                  int p = snprintf(b, sizeof b,
                                   "[mbox-chain] spu%u #%d val=0x%08X lr=0x%08X sp=0x%08X:",
                                   si, seen[si], val, (uint32_t)g_active_ctx->lr, sp);
                  uint32_t prev = 0; int found = 0;
                  for (uint32_t a = sp; a < sp + 0x2000 && found < 32; a += 4) {
                      uint32_t v = be32_load(a);
                      if (v >= 0x00010000u && v < 0x00900000u && v != prev) {
                          p += snprintf(b + p, sizeof(b) - p, " %08X", v);
                          prev = v; found++;
                      }
                  }
                  fprintf(stderr, "%s\n", b); fflush(stderr);
              } }
            spu_channel_write(&s->ctx->ch_in_mbox, val);
            spu_ch_wake(s->ctx);
            publish(s);
        }
        break;

    case SPU_RAW_SIG_NOTIFY1:
    case SPU_RAW_SIG_NOTIFY2:
        /* SPU_SIGSTAT=<n>: every n signal writes, the per-SPU tally and the
         * last value written.
         *
         * The deadlock this exists for: the R3000 spins waiting for spu4 to
         * advance a counter, spu4 polls rchcnt(SigNotify2) 1.7 BILLION times
         * without ever reading one, and the PPU thread that writes those
         * signals keeps receiving from its event queue the whole time. So
         * either the writes stop or they land and the SPU does not see them --
         * and those are opposite bugs. This counts the writes as they happen. */
        { static int s_ss = -1;
          if (s_ss < 0) { const char* e = getenv("SPU_SIGSTAT");
                          s_ss = e ? (atoi(e) > 0 ? atoi(e) : 256) : 0; }
          if (s_ss) { static unsigned long long c[8][2]; static unsigned long long n;
              static uint32_t last[8][2];
              const unsigned sp = (unsigned)(s - s_spu) & 7u;
              const int wh = (off == SPU_RAW_SIG_NOTIFY1) ? 0 : 1;
              c[sp][wh]++; last[sp][wh] = val;
              if ((++n % (unsigned long long)s_ss) == 0) {
                  fprintf(stderr, "[sigstat] %llu writes;", n);
                  for (unsigned q = 0; q < 8; q++)
                      for (int k = 0; k < 2; k++)
                          if (c[q][k])
                              fprintf(stderr, " spu%u.sig%d=%llu(last=0x%08X)",
                                      q, k + 1, c[q][k], last[q][k]);
                  fprintf(stderr, "\n"); fflush(stderr);
              } } }
        if (s->ctx) {
            int i = (off == SPU_RAW_SIG_NOTIFY1) ? 0 : 1;
            spu_channel_write(&s->ctx->ch_sig_notify[i], val);
            spu_ch_wake(s->ctx);
        }
        break;

    default:
        break;   /* NPC and the MFC proxy registers are read back from memory */
    }
}

int spu_raw_reg_load(uint32_t ea, uint32_t* out)
{
    uint32_t off;
    raw_spu* s = spu_for(ea, &off);
    if (!s || !s->ctx) return 0;

    /* Reading the outbound mailbox POPS it -- that is the whole handshake, and
     * it cannot be served out of memory. Everything else the SPU thread keeps
     * current in the window, so plain loads are correct and stay fast. */
    /* Derived registers are computed HERE, not read back from the copy publish()
     * leaves in memory. The SPU consumes its inbound mailbox on its own thread
     * without passing through any MMIO path, so a published copy goes stale the
     * moment it does -- the PPU would keep reading "no free slot", never send a
     * second word, and both sides would wait on each other forever. */
    if (off == SPU_RAW_MBOX_STATUS) {
        *out = MBOX_STATUS(s->ctx->ch_out_mbox.count,
                           SPU_RAW_IN_MBOX_DEPTH - s->ctx->ch_in_mbox.count,
                           s->ctx->ch_out_intr_mbox.count);
        return 1;
    }
    if (off == SPU_RAW_STATUS) { *out = s->ctx->status; return 1; }

    if (off == SPU_RAW_OUT_MBOX) {
        uint32_t v = spu_channel_read(&s->ctx->ch_out_mbox);
        publish(s);
        if (dbg())
            fprintf(stderr, "[spu-raw] R spu%u OUT_MBOX -> 0x%08X\n",
                    (unsigned)(s - s_spu), v);
        *out = v;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Syscalls
 * -----------------------------------------------------------------------*/
/* 160: sys_raw_spu_create(u32* id, void* attr) */
static int64_t sc_raw_spu_create(ppu_context* ctx)
{
    uint32_t id_ea = (uint32_t)ctx->gpr[3];
    for (uint32_t n = 0; n < SPU_RAW_COUNT; n++) {
        if (s_spu[n].used) continue;
        s_spu[n].used = 1;
        s_spu[n].base = SPU_RAW_BASE + n * SPU_RAW_STRIDE;

        /* Zero the problem state. Local store is left alone: the guest may have
         * already staged code there, and hardware does not clear it either. */
        memset(vm_base + s_spu[n].base + SPU_RAW_PROB_OFF, 0,
               SPU_RAW_STRIDE - SPU_RAW_PROB_OFF);
        be32_store(s_spu[n].base + SPU_RAW_MBOX_STATUS,
                   MBOX_STATUS(0, SPU_MBOX_DEPTH, 0));

        if (s_prev_out_hook == NULL && g_spu_out_mbox_hook != raw_out_mbox_hook) {
            s_prev_out_hook   = g_spu_out_mbox_hook;
            g_spu_out_mbox_hook = raw_out_mbox_hook;
        }
        if (id_ea) be32_store(id_ea, n);
        fprintf(stderr, "[spu-raw] create -> raw spu %u, window 0x%08X\n",
                n, s_spu[n].base);
        fflush(stderr);
        return CELL_OK;
    }
    return CELL_ENOMEM;
}

/* 161: sys_raw_spu_destroy(u32 id) */
static int64_t sc_raw_spu_destroy(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    if (n < SPU_RAW_COUNT) s_spu[n].used = 0;
    return CELL_OK;
}

#include "spu_intr.inc"

/* 150: sys_raw_spu_create_interrupt_tag(u32 id, u32 class_id, u32 hwthread,
 *                                       u32* intrtag) */
static int64_t sc_raw_spu_create_interrupt_tag(ppu_context* ctx)
{
    uint32_t n      = (uint32_t)ctx->gpr[3];
    uint32_t class_ = (uint32_t)ctx->gpr[4];
    uint32_t out_ea = (uint32_t)ctx->gpr[6];
    if (n >= SPU_RAW_COUNT) return CELL_EINVAL;
    s_spu[n].intrtag = 0x52000000u | (n << 8) | (class_ & 0xFF);
    if (out_ea) be32_store(out_ea, s_spu[n].intrtag);
    return CELL_OK;
}

/* 151: sys_raw_spu_set_int_mask(u32 id, u32 class_id, u64 mask) */
static int64_t sc_raw_spu_set_int_mask(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    if (n < SPU_RAW_COUNT) s_spu[n].int_mask = (uint32_t)ctx->gpr[5];
    /* SPU_DBG_MBOX=1: the mask the guest actually enables, plus each SPU's
     * outbound-mailbox depth on a timer. The image-1 SPUs signal the PPU with
     * a PLAIN `wrch SPU_WrOutMbox` (LS 0x11D4), which sets int_stat bit 0x10 --
     * so if the guest only enables 0x1|0x2 that write raises nothing and the PPU
     * has to poll. If it stopped polling, the message sits unread and the SPUs
     * spin. This prints enough to tell those apart. */
    { static int _d = -1; if (_d < 0) _d = getenv("SPU_DBG_MBOX") ? 1 : 0;
      if (_d) fprintf(stderr, "[spu-mask] spu%u class=%llu mask=0x%llX\n",
              n, (unsigned long long)ctx->gpr[4], (unsigned long long)ctx->gpr[5]); }
    return CELL_OK;
}

/* 152: sys_raw_spu_get_int_mask(u32 id, u32 class_id, u64* mask) */
static int64_t sc_raw_spu_get_int_mask(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[5];
    if (n < SPU_RAW_COUNT && out_ea) {
        be32_store(out_ea + 0, 0);
        be32_store(out_ea + 4, s_spu[n].int_mask);
    }
    return CELL_OK;
}

/* 153: sys_raw_spu_set_int_stat(u32 id, u32 class_id, u64 stat) -- write-1-to-clear */
static int64_t sc_raw_spu_set_int_stat(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    if (n < SPU_RAW_COUNT) s_spu[n].int_stat &= ~(uint32_t)ctx->gpr[5];
    return CELL_OK;
}

/* 154: sys_raw_spu_get_int_stat(u32 id, u32 class_id, u64* stat) */
static int64_t sc_raw_spu_get_int_stat(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[5];
    if (n < SPU_RAW_COUNT && out_ea) {
        be32_store(out_ea + 0, 0);
        be32_store(out_ea + 4, s_spu[n].int_stat & s_spu[n].int_mask);
    }
    return CELL_OK;
}

/* 163: sys_raw_spu_read_puint_mb(u32 id, u32* value)
 *
 * Pops the SPU's outbound INTERRUPT mailbox. The image's interrupt thread does
 *   get_int_stat(id, 2, &st); if (st & 1) { read_puint_mb(id, &v);
 *                                           set_int_stat(id, 2, 1); }
 * so this is the read half of the class-2 mailbox interrupt. */
static int64_t sc_raw_spu_read_puint_mb(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t out_ea = (uint32_t)ctx->gpr[4];
    if (n >= SPU_RAW_COUNT || !s_spu[n].ctx) return CELL_EINVAL;
    uint32_t v = spu_channel_read(&s_spu[n].ctx->ch_out_intr_mbox);
    if (out_ea) be32_store(out_ea, v);
    publish(&s_spu[n]);
    if (dbg())
        fprintf(stderr, "[spu-raw] read_puint_mb spu%u -> 0x%08X\n", n, v);
    return CELL_OK;
}

/* sys_raw_spu_image_load(int id, sys_spu_image_t* img)  -- NID 0xB995662E
 *
 * THE step that was missing. It is not a syscall: libsysutil exports it, so a
 * stub NID left local store full of zeros and the SPU "ran" straight into them,
 * halting at pc 0 -- which looks exactly like a lifting bug and is not one.
 *
 * The descriptor is the one hle_sys_spu_image_import built:
 *   +0x00 type  +0x04 entry  +0x08 segment array EA  +0x0C segment count
 * and each 0x18-byte segment is
 *   +0x00 type (1 = COPY, 2 = FILL)  +0x04 LS address  +0x08 size
 *   +0x10/+0x14 source EA as a u64.
 * Copying is a plain host memcpy: local store IS guest memory for a raw SPU, and
 * both sides are big-endian, so nothing is byte-swapped on the way in. */
static void hle_raw_spu_image_load(ppu_context* ctx)
{
    uint32_t n      = (uint32_t)ctx->gpr[3];
    uint32_t img_ea = (uint32_t)ctx->gpr[4];
    if (n >= SPU_RAW_COUNT || !img_ea || !vm_base) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_EINVAL;
        return;
    }
    uint32_t base  = SPU_RAW_BASE + n * SPU_RAW_STRIDE;
    uint32_t entry = be32_load(img_ea + 0x04);
    uint32_t segs  = be32_load(img_ea + 0x08);
    uint32_t nsegs = be32_load(img_ea + 0x0C);

    uint32_t copied = 0;
    for (uint32_t i = 0; i < nsegs && i < 32; i++) {
        uint32_t sg   = segs + i * 0x18;
        uint32_t type = be32_load(sg + 0x00);
        uint32_t lsa  = be32_load(sg + 0x04) & SPU_LS_MASK;
        uint32_t size = be32_load(sg + 0x08);
        uint32_t src  = be32_load(sg + 0x14);
        if (size > SPU_LS_SIZE - lsa) size = SPU_LS_SIZE - lsa;
        if (type == 1 && src) { memcpy(vm_base + base + lsa, vm_base + src, size); copied += size; }
        else if (type == 2)   { memset(vm_base + base + lsa, 0, size); }
    }
    be32_store(base + SPU_RAW_NPC, entry);

    fprintf(stderr, "[spu-raw] image_load spu%u: %u segments, %u bytes into LS, "
                    "NPC=0x%05X\n", n, nsegs, copied, entry);
    fflush(stderr);
    ctx->gpr[3] = CELL_OK;
}

/* sys_spu_image_close(sys_spu_image_t*) -- NID 0xE0DA8EFD. The descriptor lives
 * in a runtime bump arena that is never reclaimed, so there is nothing to free;
 * it exists so the guest's own error checking passes. */
static void hle_spu_image_close(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

void sys_raw_spu_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, 84, sc_interrupt_thread_establish);
    lv2_syscall_register(tbl, 88, sc_interrupt_thread_eoi);
    lv2_syscall_register(tbl, 89, sc_interrupt_thread_disestablish);
    lv2_syscall_register(tbl, 150, sc_raw_spu_create_interrupt_tag);
    lv2_syscall_register(tbl, 151, sc_raw_spu_set_int_mask);
    lv2_syscall_register(tbl, 152, sc_raw_spu_get_int_mask);
    lv2_syscall_register(tbl, 153, sc_raw_spu_set_int_stat);
    lv2_syscall_register(tbl, 154, sc_raw_spu_get_int_stat);
    lv2_syscall_register(tbl, 160, sc_raw_spu_create);
    lv2_syscall_register(tbl, 161, sc_raw_spu_destroy);
    lv2_syscall_register(tbl, 163, sc_raw_spu_read_puint_mb);

    /* Exported by libsysutil, not syscalls -- and sys_raw_spu_image_load is the
     * one that puts the SPU's code in local store, so a stub here is the
     * difference between an SPU that runs and one that executes 256 KB of
     * zeros. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_raw_spu_image_load"),
                         "sys_raw_spu_image_load", hle_raw_spu_image_load);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spu_image_close"),
                         "sys_spu_image_close", hle_spu_image_close);
}
