/*
 * ps3recomp - SPU DMA engine (MFC)
 *
 * The Memory Flow Controller (MFC) handles DMA transfers between an SPU's
 * 256 KB local store and the main memory (EA space).  Each SPU has a 16-entry
 * MFC command queue.
 *
 * Supported operations:
 *   - DMA get/put  (local store <-> main memory)
 *   - DMA list commands (scatter/gather)
 *   - Tag group management and synchronization
 *   - Barrier and fence
 */

#ifndef SPU_DMA_H
#define SPU_DMA_H

#include "spu_context.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* vm_base;

/* Guard against a guest DMA whose effective address lands in reserved-but-
 * uncommitted guest memory (the 4 GB VM is MEM_RESERVE; only main mem / RSX /
 * SPU / lv2-heap / stack pages are committed). A garbage EA — e.g. one the SPURS
 * kernel computes from an incomplete context during bring-up — must be treated
 * as a failed DMA, NOT crash the host emulator with an access violation. On
 * Windows we query the page state; the whole [ea, ea+size) range must be
 * committed and accessible. Returns 1 if the range is safe to memcpy. */
static inline int mfc_ea_range_committed(uint64_t ea, uint32_t size)
{
    uint32_t e = (uint32_t)ea;
    if (size == 0) return 0;
    if ((uint64_t)e + (uint64_t)size > 0x100000000ull) return 0;   /* past 4 GB */
#ifdef _WIN32
    if (!vm_base) return 0;
    uint8_t* p = vm_base + e;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    /* If the range spans into the next region, verify the last byte's page too. */
    uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if ((uintptr_t)p + size > region_end) {
        MEMORY_BASIC_INFORMATION mbi2;
        if (VirtualQuery(p + size - 1, &mbi2, sizeof(mbi2)) == 0) return 0;
        if (mbi2.State != MEM_COMMIT) return 0;
        if (mbi2.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    }
#endif
    return 1;
}

/* MFC queue depth */
#define MFC_QUEUE_DEPTH     16

/* Maximum DMA transfer size per element */
#define MFC_MAX_DMA_SIZE    (16 * 1024)

/* ---------------------------------------------------------------------------
 * MFC command entry
 * -----------------------------------------------------------------------*/
typedef struct mfc_cmd {
    uint32_t lsa;       /* Local store address */
    uint64_t ea;        /* Effective (main memory) address: EAH << 32 | EAL */
    uint32_t size;      /* Transfer size in bytes */
    uint32_t tag;       /* Tag group ID (0-31) */
    uint32_t cmd;       /* Command opcode (MFC_PUT_CMD, MFC_GET_CMD, etc.) */

    /* Status */
    int      active;    /* 1 = pending, 0 = completed/free */
} mfc_cmd;

/* DMA list element (used by PUTL/GETL) */
typedef struct mfc_list_element {
    /* In PS3 memory these are big-endian:
     *   bits  0-14 : reserved (notify/stall flags in upper halfword)
     *   bit   15   : stall-and-notify
     *   bits 16-31 : transfer size (low 16 bits)
     *   bits 32-63 : effective address low 32 bits
     */
    uint32_t size_and_flags;
    uint32_t eal;
} mfc_list_element;

/* ---------------------------------------------------------------------------
 * MFC DMA engine state
 * -----------------------------------------------------------------------*/
typedef struct mfc_engine {
    mfc_cmd     queue[MFC_QUEUE_DEPTH];
    uint32_t    queue_count;

    /* Per-tag completion tracking (bitmask: bit N = tag N completed) */
    uint32_t    tag_completed;
} mfc_engine;

/* External: pointer to host mapping of PS3 main memory.
 * Must be set by the VM manager before any DMA. */
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------------------*/
static inline void mfc_engine_init(mfc_engine* mfc)
{
    memset(mfc, 0, sizeof(*mfc));
    mfc->tag_completed = 0xFFFFFFFF; /* all tags idle = completed */
}

/* ---------------------------------------------------------------------------
 * Core DMA transfer (synchronous for recompiled code)
 *
 * In real hardware this would be asynchronous.  For a static recompiler the
 * DMA is executed immediately; the tag status is updated after completion.
 * -----------------------------------------------------------------------*/

static inline int mfc_is_get(uint32_t cmd)
{
    return (cmd & 0x40) != 0; /* GET family has bit 6 set */
}

static inline int mfc_is_put(uint32_t cmd)
{
    return (cmd & 0x20) != 0 && !mfc_is_get(cmd);
}

static inline int mfc_is_list(uint32_t cmd)
{
    return (cmd & 0x04) != 0; /* list variants have bit 2 set */
}

static inline int mfc_is_barrier(uint32_t cmd)
{
    return (cmd & 0x01) != 0; /* barrier variants have bit 0 set */
}

static inline int mfc_is_fence(uint32_t cmd)
{
    return (cmd & 0x02) != 0; /* fence variants have bit 1 set */
}

/*
 * Execute a single DMA transfer between local store and main memory.
 * Returns 0 on success, -1 on error.
 */
static inline int mfc_do_transfer(spu_context* spu, uint32_t lsa, uint64_t ea,
                                   uint32_t size, uint32_t cmd)
{
    /* Validate size */
    if (size == 0 || size > MFC_MAX_DMA_SIZE)
        return -1;

    /* Mask LSA to local store range */
    lsa &= SPU_LS_MASK;

    /* Reject transfers that would overrun the 256 KB local store buffer (a real
     * SPU MFC requires the LS range to be valid; an out-of-range lsa+size here
     * would corrupt host heap past spu->ls). */
    if ((uint64_t)lsa + (uint64_t)size > (uint64_t)SPU_LS_SIZE)
        return -1;

    /* Reject transfers whose main-memory range is not committed guest memory,
     * so a garbage EA (e.g. from an incomplete SPURS context) is a failed DMA
     * rather than a host segfault. */
    if (!mfc_ea_range_committed(ea, size)) {
        static int s_warned = 0;
        if (s_warned++ < 32)
            fprintf(stderr, "[spu-dma] SKIP %s lsa=0x%05X ea=0x%08X size=%u "
                    "(EA not committed -- bad/garbage DMA target)\n",
                    mfc_is_get(cmd) ? "GET" : "PUT", lsa, (uint32_t)ea, size);
        return -1;
    }

    uint8_t* ls_ptr = &spu->ls[lsa];
    uint8_t* ea_ptr = vm_base + (uint32_t)ea; /* PS3 uses 32-bit effective addresses for SPU DMA */

#ifdef SPU_DMA_LOG
    { extern int g_spu_dma_log; if (g_spu_dma_log-- > 0)
        fprintf(stderr, "[spu-dma] %s lsa=0x%05X ea=0x%08X size=%u\n",
                mfc_is_get(cmd) ? "GET" : "PUT", lsa, (uint32_t)ea, size); }
#endif
    { static int s_t = -1; static int s_img = -2;
      if (s_t < 0) s_t = getenv("YDKJ_DMATRACE") ? 1 : 0;
      if (s_img == -2) { const char* e = getenv("YDKJ_DMA_IMG"); s_img = e ? atoi(e) : -1; }
      /* YDKJ_DMA_IMG=N: trace ONLY image N, uncapped (the 300-cap otherwise fills
       * with the always-running PM's DMA and hides a late image entirely). */
      if (s_t) {
        if (s_img >= 0) {
            if (spu->image_id == s_img)
                fprintf(stderr, "[dmatrace] %s lsa=0x%05X ea=0x%08X size=%u img=%d\n",
                        mfc_is_get(cmd) ? "GET" : "PUT", lsa, (uint32_t)ea, size, spu->image_id);
        } else { static int _n = 0; if (_n++ < 300)
            fprintf(stderr, "[dmatrace] %s lsa=0x%05X ea=0x%08X size=%u img=%d\n",
                    mfc_is_get(cmd) ? "GET" : "PUT", lsa, (uint32_t)ea, size, spu->image_id); }
      } }
    /* Detect the cri task actually DECODING: a GET of the real video payload
     * (large, from a non-context EA) as opposed to the 64-byte context handshake
     * DMAs. Set a flag the dispatcher polls to know the task got real work. */
    { extern int g_cri_video_dma;
      if (spu->image_id == 22 && mfc_is_get(cmd) && size > 0x100)
          g_cri_video_dma = 1; }
    if (mfc_is_get(cmd)) {
        /* GET: main memory -> local store */
        memcpy(ls_ptr, ea_ptr, size);
    } else if (mfc_is_put(cmd)) {
        /* PUT: local store -> main memory */
        memcpy(ea_ptr, ls_ptr, size);
        /* POISON-DMA detector: does an SPU PUT write the singleton object region
         * (0x40003000-0x40005000) or carry the 0xC708C708 poison? This host-side
         * memcpy bypasses vm_write32/AWATCH, so it's the prime suspect for the
         * garbage fn-ptr fields that cause the 0xC708C708 vcall crash. */
        { uint32_t e = (uint32_t)ea;
          int hitregion = (e < 0x40005000u && e + size > 0x40003000u);
          int haspoison = 0;
          for (uint32_t o = 0; o + 4 <= size; o += 4) {
              uint32_t w; memcpy(&w, (const char*)ls_ptr + o, 4);
              if (w == 0x08C708C7u) { haspoison = 1; break; } /* 0xC708C708 BE in LS bytes */
          }
          if (hitregion || haspoison) {
              static int _n = 0;
              if (_n++ < 12)
                  fprintf(stderr, "[POISON-DMA] img%u PUT ea=0x%08X size=0x%X lsa=0x%X hitregion=%d haspoison=%d\n",
                          spu->image_id, e, size, lsa, hitregion, haspoison);
          } }
    }

    return 0;
}

/*
 * Execute a DMA list command (scatter/gather).
 * The list resides in the SPU's local store at `lsa`.
 * Each list element describes a (size, EA) pair for a sub-transfer.
 */
static inline int mfc_do_list_transfer(spu_context* spu, uint32_t list_lsa,
                                        uint64_t ea_base, uint32_t list_size,
                                        uint32_t cmd)
{
    /* list_size is in bytes; each element is 8 bytes */
    uint32_t num_elements = list_size / 8;
    uint32_t base_cmd = cmd & ~0x04u; /* strip the 'list' bit to get base GET/PUT */

    for (uint32_t i = 0; i < num_elements; i++) {
        uint32_t elem_lsa = (list_lsa + i * 8) & SPU_LS_MASK;

        /* Read list element from local store (big-endian) */
        uint32_t size_and_flags = spu_ls_read32(spu, elem_lsa);
        uint32_t eal = spu_ls_read32(spu, elem_lsa + 4);

        uint32_t xfer_size = size_and_flags & 0x7FFF; /* low 15 bits */
        int stall_notify = (size_and_flags >> 15) & 1;

        uint64_t ea = (ea_base & 0xFFFFFFFF00000000ull) | eal;

        /* Calculate target LSA: for list commands, the data starts at
         * the address given by the MFC_LSA channel and accumulates. */
        int rc = mfc_do_transfer(spu, spu->mfc_lsa, ea, xfer_size, base_cmd);
        if (rc != 0) return rc;

        spu->mfc_lsa += xfer_size;

        if (stall_notify) {
            /* In a full emulator we would raise a stall-and-notify event.
             * For recompiled code, we just continue. */
        }
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Enqueue an MFC command (called when SPU writes to MFC_Cmd channel)
 * -----------------------------------------------------------------------*/
static inline int mfc_enqueue(mfc_engine* mfc, spu_context* spu)
{
    if (mfc->queue_count >= MFC_QUEUE_DEPTH)
        return -1; /* queue full */

    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag;
    uint32_t cmd  = spu->ls[0]; /* overridden below */

    /* The cmd was just written to the MFC_Cmd channel -- we receive it
     * as a parameter to the channel write handler.  For this inline API,
     * the caller should pass it.  We use a wrapper below. */
    (void)cmd;
    return 0;
}

/*
 * Submit and immediately execute an MFC command.
 * This is the main entry point called when the SPU writes to MFC_Cmd.
 */
static inline int mfc_submit(mfc_engine* mfc, spu_context* spu, uint32_t cmd)
{
    uint32_t lsa  = spu->mfc_lsa;
    uint64_t ea   = ((uint64_t)spu->mfc_eah << 32) | spu->mfc_eal;
    uint32_t size = spu->mfc_size;
    uint32_t tag  = spu->mfc_tag & 0x1F;
    int rc = 0;

    /* cri build (YDKJ_CRI_CHAIN): when the kernel DMA-loads the TASKSET policy
     * module (libsre guest 0x30023680) to LS 0xA00, switch this SPU's image to 23
     * so subsequent indirect branches to 0xA00 resolve lift_tsp (taskset policy)
     * instead of image-22's lift_pol (sys-service). The cri task (0x3000+) is
     * registered under both 22 and 23, so it still resolves after the switch. */
    if (lsa == 0xA00u && (uint32_t)ea == 0x30023680u) {
        static int s_cc = -1; if (s_cc < 0) s_cc = getenv("YDKJ_CRI_CHAIN") ? 1 : 0;
        if (s_cc) {
            spu->image_id = 23;
            fprintf(stderr, "[cri-chain] taskset policy DMA'd to LS 0xA00 -> SPU image -> 23\n");
            /* YDKJ_CRI_R4: the taskset policy entry (tsp func_00000A00) writes r4
             * into SpursTasksetContext.taskset @LS 0x27B8 (shufb gpr[4] -> +8), so
             * r4 MUST be the CellSpursTaskset EA (per RPCS3 cellSpursSpu.cpp). The
             * kernel->policy handoff doesn't convey it in our path, so the policy
             * DMAs the taskset from garbage -> reads waiting!=0 -> wrong resume
             * path -> savedContextLr=0. Inject r4 = taskset EA (0x0F000000) here.
             * u64 pref-doubleword: bytes0-3=0, bytes4-7=0x0F000000 -> _u32[0]=0,
             * _u32[1]=EA. Gated (default on with CRI_CHAIN unless YDKJ_NO_CRI_R4). */
            if (!getenv("YDKJ_NO_CRI_R4")) {
                spu->gpr[4]._u32[0] = 0x00000000u;
                spu->gpr[4]._u32[1] = 0x0F000000u;   /* taskset EA (matches CRI_CHAIN TSEA) */
                fprintf(stderr, "[cri-chain] injected r4 = taskset EA 0x0F000000 (-> ctxt->taskset @LS 0x27B8)\n");
            }
            fflush(stderr);
        }
    }

    /* Mark tag as in-progress */
    mfc->tag_completed &= ~(1u << tag);

    /* Handle sync/barrier commands */
    if (cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD) {
        /* For synchronous emulation these are no-ops; all prior DMAs
         * have already completed. */
        mfc->tag_completed |= (1u << tag);
        return 0;
    }

    /* DMA trace for the cri task (image 22): log each transfer's LS/EA/size so we
     * can see the work-fetch GET (func_000040F0) that precedes the branch-to-0 and
     * whether its source EA holds valid work-queue data. Env YDKJ_DMATRACE. */
    {
        static int64_t dt=-2; if (dt==-2){ const char* e=getenv("YDKJ_DMATRACE"); dt=e?1:0; }
        if (dt && (spu->image_id==22 || spu->image_id==23)) {
            static int _n=0; if (_n++ < 160)
                fprintf(stderr, "[DMA] img%d cmd=0x%02X lsa=0x%05X ea=0x%09llX size=0x%X tag=%u\n",
                        spu->image_id, cmd, lsa, (unsigned long long)ea, size, tag);
            /* YDKJ_CRI_R4 diag: when the policy issues the mis-computed context
             * DMA (img23, lsa=0x2780, garbage high EA), dump the loaded
             * SpursTasksetContext (LS 0x2700..0x27E0) so we can see which field
             * fed the bad EA. taskset@0x27B8, kernelMgmt@0x27C0, syscall@0x27C4. */
            if (spu->image_id==23 && lsa==0x2780u && ((ea>>32)&0xFFFF)) {
                static int _d=0; if (_d++ < 3) {
                    fprintf(stderr, "[cri-r4] BAD-DMA ea=0x%09llX; SpursTasksetContext LS[0x2700..0x27E0]:\n",
                            (unsigned long long)ea);
                    for (uint32_t o=0x2700; o<0x27E0; o+=16)
                        fprintf(stderr, "   LS[0x%04X]: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", o,
                            spu->ls[o+0],spu->ls[o+1],spu->ls[o+2],spu->ls[o+3], spu->ls[o+4],spu->ls[o+5],spu->ls[o+6],spu->ls[o+7],
                            spu->ls[o+8],spu->ls[o+9],spu->ls[o+10],spu->ls[o+11], spu->ls[o+12],spu->ls[o+13],spu->ls[o+14],spu->ls[o+15]);
                }
            }
            /* kernel bootstrap DMA: dump LS[0x1C0] (=r17, the packed instance EA via
             * the entry's cdd/cwd/shufb) to check if the EA got byte-mangled. */
            if (cmd==0x40 && lsa==0x3FFE0) {
                static int _k=0; if (_k++ < 3) {
                    const uint8_t* p=&spu->ls[0x1C0];
                    fprintf(stderr, "[DMA] LS[0x1C0] (r17, packed instance) = %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                        p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
                    /* byte-order hypothesis: correct offset is 0xD00 (not 0xD0000).
                     * Dump guest mem at instance+0xD00 (the SPU-correct EA) vs +0xD0000. */
                    extern uint8_t* vm_base;
                    uint32_t inst = (uint32_t)(ea - 0xD0000u);  /* recover instance base */
                    for (uint32_t off2=0xD00; off2<=0xD0000; off2+= (0xD0000-0xD00)) {
                        const uint8_t* q = vm_base + inst + off2;
                        fprintf(stderr, "[DMA]   guest[inst+0x%05X = 0x%08X]: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                            off2, inst+off2, q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15]);
                    }
                }
            }
        }
    }

    /* Policy-module observation trace: every SPURS PM run's first transfers,
     * always on (policy runs are rare and their DMA pattern is the primary
     * bring-up signal: joblist fetch, job-module loads, output stores). */
    if (spu->policy_mode) {
        static int _pn = 0;
        if (_pn < 96)
            fprintf(stderr, "[spurs-pm] DMA cmd=0x%02X lsa=0x%05X ea=0x%09llX size=0x%X tag=%u\n",
                    cmd, lsa, (unsigned long long)ea, size, tag);
        else if (_pn == 96)
            fprintf(stderr, "[spurs-pm] DMA trace suppressed from here\n");
        _pn++;
    }

    /* Execute the transfer */
    if (mfc_is_list(cmd)) {
        rc = mfc_do_list_transfer(spu, lsa, ea, size, cmd);
    } else {
        rc = mfc_do_transfer(spu, lsa, ea, size, cmd);
    }

    /* After a cri-task GET, dump the bytes it just read (the task context) so we
     * can tell if eaContext holds valid SPURS work data or garbage. */
    {
        static int64_t dt2=-2; if (dt2==-2){ const char* e=getenv("YDKJ_DMATRACE"); dt2=e?1:0; }
        if (dt2 && spu->image_id==22 && cmd==0x40 /*GET*/ && size<=0x80) {
            static int _g=0; if (_g++ < 6) {
                const uint8_t* p = spu->ls + (lsa & 0x3FFFF);
                fprintf(stderr, "[DMA] GET data @LS0x%05X (from ea=0x%09llX):", lsa, (unsigned long long)ea);
                for (uint32_t i=0;i<size && i<0x40;i+=4)
                    fprintf(stderr, " %02X%02X%02X%02X", p[i],p[i+1],p[i+2],p[i+3]);
                fprintf(stderr, "\n");
            }
        }
    }

    /* Mark tag as completed */
    mfc->tag_completed |= (1u << tag);

    return rc;
}

/* ---------------------------------------------------------------------------
 * Tag group synchronization
 * -----------------------------------------------------------------------*/

/* Tag update types (values written to MFC_WrTagUpdate) */
#define MFC_TAG_UPDATE_IMMEDIATE  0  /* return status immediately */
#define MFC_TAG_UPDATE_ANY        1  /* wait for any tag in mask */
#define MFC_TAG_UPDATE_ALL        2  /* wait for all tags in mask */

/*
 * Query tag status.  Returns the bitmask of completed tags that match
 * the tag mask.  In synchronous mode, all submitted DMAs are already
 * complete, so this just returns the intersection.
 */
static inline uint32_t mfc_read_tag_status(const mfc_engine* mfc, uint32_t tag_mask)
{
    return mfc->tag_completed & tag_mask;
}

/*
 * Poll for tag completion.
 * update_type: 0 = immediate, 1 = any, 2 = all.
 * Returns the completed tag mask.  In synchronous mode, always succeeds.
 */
static inline uint32_t mfc_tag_wait(const mfc_engine* mfc, uint32_t tag_mask,
                                     uint32_t update_type)
{
    uint32_t completed = mfc->tag_completed & tag_mask;

    switch (update_type) {
    case MFC_TAG_UPDATE_IMMEDIATE:
        return completed;
    case MFC_TAG_UPDATE_ANY:
        /* In async mode we would block until any bit is set.
         * Synchronous: always returns immediately. */
        return completed;
    case MFC_TAG_UPDATE_ALL:
        /* Block until all bits in mask are set. */
        return completed;
    default:
        return completed;
    }
}

/* ---------------------------------------------------------------------------
 * Convenience: process an SPU channel write for MFC-related channels.
 * -----------------------------------------------------------------------*/
static inline void mfc_channel_write(mfc_engine* mfc, spu_context* spu,
                                      uint32_t channel, uint32_t value)
{
    switch (channel) {
    case MFC_LSA:
        spu->mfc_lsa = value;
        break;
    case MFC_EAH:
        spu->mfc_eah = value;
        break;
    case MFC_EAL:
        spu->mfc_eal = value;
        break;
    case MFC_Size:
        spu->mfc_size = value;
        break;
    case MFC_TagID:
        spu->mfc_tag = value & 0x1F;
        break;
    case MFC_Cmd:
        mfc_submit(mfc, spu, value);
        break;
    case MFC_WrTagMask:
        spu->mfc_tag_mask = value;
        break;
    case MFC_WrTagUpdate:
        spu->mfc_tag_status = mfc_tag_wait(mfc, spu->mfc_tag_mask, value);
        break;
    case MFC_WrListStallAck:
        /* Acknowledge stall -- no-op in synchronous mode */
        break;
    default:
        break;
    }
}

static inline uint32_t mfc_channel_read(mfc_engine* mfc, spu_context* spu,
                                         uint32_t channel)
{
    switch (channel) {
    case MFC_RdTagStat:
        return spu->mfc_tag_status;
    case MFC_RdTagMask:
        return spu->mfc_tag_mask;
    case MFC_RdListStallStat:
        return 0; /* no stalls in synchronous mode */
    case MFC_RdAtomicStat:
        /* Result of the last atomic line op (GETLLAR/PUTLLC/PUTLLUC), set by
         * spu_mfc_atomic(): 0 = PUTLLC_SUCCESS, 1 = PUTLLC_FAILURE (line moved,
         * SPU must retry). Honoring this is what keeps the SPURS lock-free queue
         * consistent across concurrent SPU kernel threads. */
        return spu->atomic_stat;
    default:
        return 0;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPU_DMA_H */
