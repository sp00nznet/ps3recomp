/*
 * ps3recomp - cellAdec HLE implementation
 *
 * Stub audio decoder. Accepts AU data and delivers AUDONE callbacks.
 * Actual decoding (AAC, ATRAC3+, etc.) requires integration with
 * an audio codec library (e.g., FFmpeg).
 */

#include "cellAdec.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/ppu/ppu_context.h" /* g_active_ctx -> the guest lr */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */

/* CellAdecPcmItem as the guest reads it, from RPCS3 Modules/cellAdec.h:339.
 * ps1_netemu copies the whole thing out with six 8-byte loads from offsets
 * 0x00..0x28 (at 0x000ED964), and that copy is what confirms these offsets
 * rather than a guess about padding: auInfo lands at 0x18, not 0x14, because it
 * contains a u64. The block must be 0x30 readable GUEST bytes. */
#define PCMITEM_PCM_HANDLE   0x00u
#define PCMITEM_STATUS       0x04u
#define PCMITEM_START_ADDR   0x08u
#define PCMITEM_SIZE         0x0Cu
#define PCMITEM_BSI_INFO     0x10u
#define PCMITEM_AU_START     0x18u
#define PCMITEM_AU_SIZE      0x1Cu
#define PCMITEM_AU_PTS_HI    0x20u
#define PCMITEM_AU_PTS_LO    0x24u
#define PCMITEM_AU_USERDATA  0x28u
#define PCMITEM_BYTES        0x30u

/* ponytail: one fixed-size silence buffer per handle, 1024 stereo float samples
 * -- the ATRAC3 frame this title (codecType 5) decodes. The ceiling is that it
 * is SILENCE and always this size; wire a real decoder in here when the audio
 * itself matters, without changing the guest-visible contract. */
#define ADEC_PCM_SAMPLES  1024u
#define ADEC_PCM_BYTES    (ADEC_PCM_SAMPLES * 2u * 4u)

/* Guest-visible scratch, in the HLE inject window rather than from the guest's
 * own heap. Taking 8 KB out of that heap in cellAdecOpen stalled the title
 * before it even reached cellAdecStartSeq -- ps1_netemu allocates its own
 * buffers from the same bump allocator, and perturbing it is not worth it for
 * two fixed-size blocks whose lifetime is the whole process.
 *
 * +0x40000 is clear of everything else in the window: labels +0x0000, control
 * +0x2000, callback +0x2F00, the offset tables +0x3000/+0x5000, and sys_rsx's
 * device/driver-info/reports pages at +0x30000/+0x31000/+0x38000. */
#define ADEC_SCRATCH_BASE   (VM_HLE_INJECT_BASE + 0x40000u)
#define ADEC_SCRATCH_STRIDE 0x4000u
#define ADEC_ITEM_EA(h)     (ADEC_SCRATCH_BASE + (u32)(h) * ADEC_SCRATCH_STRIDE)
#define ADEC_PCM_EA(h)      (ADEC_ITEM_EA(h) + 0x100u)
/* The codec info block the PcmItem's bsiInfo points to, in the gap between
 * the 0x30-byte item and the PCM buffer. */
#define ADEC_BSI_EA(h)      (ADEC_ITEM_EA(h) + 0x40u)
#define ADEC_BSI_BYTES      0x40u

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_ADEC 4

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;         /* guest EA of the callback's OPD */
    u32 cbArg;          /* guest EA handed back to it     */
    int seqStarted;
    u32 itemEa;         /* guest EA of the 0x30-byte CellAdecPcmItem */
    u32 pcmEa;          /* guest EA of the PCM buffer it points at    */
    int hasPcm;
    u32 auCount;        /* total AUs decoded */
} AdecSlot;

static AdecSlot s_adec[MAX_ADEC];

/* Fire one guest callback.
 *
 * The guest's cbFunc is a guest EA naming an OPD, so it goes through the
 * recompiled code's dispatcher. It used to be cast to a host function pointer
 * and called directly, which jumped to the guest address as host code:
 *
 *     [CRASH] code=0xC0000005 rip=00000000001B5F00
 *     [CRASH] last HLE NID (cellAdecDecodeAu)
 *
 * 0x1B5F00 is the guest OPD. That crash is what this function exists to stop.
 *
 * ponytail: dispatched SYNCHRONOUSLY on the caller's thread rather than from a
 * decoder thread as lv2 does. The ceiling is a guest callback that blocks
 * waiting on the thread that called DecodeAu -- give this its own thread if a
 * title ever deadlocks here. */
static void adec_notify(CellAdecHandle handle, u32 msg_type, s32 msg_data)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) return;
    const AdecSlot* a = &s_adec[handle];
    { static int _n = 0;
      if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 4)
          printf("[cellAdec] notify h=%u msg=%u data=0x%X cb=0x%08X arg=0x%08X caller=%d\n",
                 handle, msg_type, (unsigned)msg_data, a->cbFunc, a->cbArg,
                 g_ps3_guest_caller ? 1 : 0); }
    if (!a->cbFunc || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(a->cbFunc, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)a->cbArg, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* cellAdecQueryAttr: how much work memory the decoder wants, before Open.
 * CellAdecAttr (guest, big-endian): +0 u32 workMemSize, +4 u32 adecVerUpper,
 * +8 u32 adecVerLower. Decoding is host-side and never touches the buffer;
 * see cellVdecQueryAttr. */
s32 cellAdecQueryAttr(const CellAdecType* type, void* attr)
{
    u32 ea = (u32)(uintptr_t)attr;
    if (!type || !ea)
        return (s32)CELL_ADEC_ERROR_ARG;
    vm_write32(ea + 0x0, 0x40000);    /* workMemSize: 256 KB */
    vm_write32(ea + 0x4, 0x00010000); /* adecVerUpper */
    vm_write32(ea + 0x8, 0);          /* adecVerLower */
    return CELL_OK;
}

/* cellAdecOpen(type, res, cb, handle) -- FOUR arguments.
 *
 * This took five, splitting the guest's CellAdecCb struct into cbFunc + cbArg,
 * which pushed `handle` off r6 onto r7. r7 held whatever happened to be there, so
 * the null check failed and every Open returned CELL_ADEC_ERROR_ARG. ps1_netemu
 * opens a decoder for CD-DA/XA audio inside its CD-ROM constructor and gives up on
 * the error -- so the disc was never mounted and the emulator sat in its run loop
 * with nothing to run. Signature confirmed against RPCS3 (Modules/cellAdec.cpp).
 *
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. */
s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                 const CellAdecCb* cb, CellAdecHandle* handle)
{
    (void)res;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* audioCodecType */
    u32 cb_ea      = (u32)(uintptr_t)cb;
    u32 handle_ea  = (u32)(uintptr_t)handle;
    printf("[cellAdec] Open(codecType=%u, cb=0x%08X, handle=0x%08X)\n",
           codec_type, cb_ea, handle_ea);

    if (!type || !handle_ea)
        return (s32)CELL_ADEC_ERROR_ARG;

    for (int i = 0; i < MAX_ADEC; i++) {
        if (!s_adec[i].in_use) {
            memset(&s_adec[i], 0, sizeof(AdecSlot));
            s_adec[i].in_use    = 1;
            s_adec[i].codecType = codec_type;
            if (cb_ea) {
                s_adec[i].cbFunc = vm_read32(cb_ea + 0);
                s_adec[i].cbArg  = vm_read32(cb_ea + 4);
            }
            /* The PcmItem is handed to the guest BY POINTER, so it cannot live
             * in host memory -- returning &host_struct is what crashed here
             * once already. */
            s_adec[i].itemEa = ADEC_ITEM_EA(i);
            s_adec[i].pcmEa  = ADEC_PCM_EA(i);
            vm_write32(handle_ea, (u32)i);
            printf("[cellAdec] Open -> handle=%u item=0x%08X pcm=0x%08X\n",
                   i, s_adec[i].itemEa, s_adec[i].pcmEa);
            return CELL_OK;
        }
    }
    return (s32)CELL_ADEC_ERROR_BUSY;
}

s32 cellAdecClose(CellAdecHandle handle)
{
    printf("[cellAdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellAdecStartSeq(CellAdecHandle handle, void* param)
{
    (void)param;
    printf("[cellAdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].seqStarted = 1;
    return CELL_OK;
}

s32 cellAdecEndSeq(CellAdecHandle handle)
{
    /* ADEC_WHO=1: name the guest function driving this. The title calls EndSeq
     * hundreds of times against a single StartSeq, and the image has eight
     * EndSeq call sites -- the backtrace says which one. */
    { static int _n = -1;
      if (_n < 0) _n = getenv("ADEC_WHO") ? 0 : -2;
      if (_n >= 0 && _n < 8) { _n++;
          /* The guest lr, not a host backtrace. ppu_guest_caller maps host
           * frames to the nearest lifted function, and here it landed on
           * func_00013040 -- a single `blr`, so plainly a mis-attribution.
           * lr is written by the `bl` to the import stub, so it is exactly
           * (call site + 4) for a call like this one. */
          extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
          printf("[cellAdec] EndSeq from guest lr=0x%08X tid=%llu\n",
                 g_active_ctx ? (u32)g_active_ctx->lr : 0u,
                 g_active_ctx ? (unsigned long long)g_active_ctx->thread_id : 0ull); } }
    printf("[cellAdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].seqStarted = 0;

    adec_notify(handle, CELL_ADEC_MSG_TYPE_SEQDONE, CELL_OK);

    return CELL_OK;
}

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];

    printf("[cellAdec] DecodeAu(handle=%u, addr=0x%X, size=%u)\n",
           handle, vm_read32(GUEST_EA(auInfo) + (u32)offsetof(CellAdecAuInfo, startAddr)),
           vm_read32(GUEST_EA(auInfo) + (u32)offsetof(CellAdecAuInfo, size)));

    /* Step 1: report the AU consumed. msgData is the AU INFO ADDRESS, not a
     * status -- see RPCS3 Modules/cellAdec.cpp:1345. A decoder that tracks its
     * outstanding AUs by address gets a null back if this is CELL_OK. */
    adec_notify(handle, CELL_ADEC_MSG_TYPE_AUDONE, (s32)GUEST_EA(auInfo));

    /* Step 2: Generate PCMOUT callback with dummy PCM info.
     * Without FFmpeg, we produce silence. But games that check for
     * decode completion via callbacks will proceed correctly. */
    a->auCount++;
    {
        const u32 au = GUEST_EA(auInfo);
        const u32 it = a->itemEa;
        for (u32 o = 0; o < ADEC_PCM_BYTES; o += 4) vm_write32(a->pcmEa + o, 0);

        vm_write32(it + PCMITEM_PCM_HANDLE, (u32)handle);
        vm_write32(it + PCMITEM_STATUS,     0);              /* CELL_OK */
        vm_write32(it + PCMITEM_START_ADDR, a->pcmEa);
        vm_write32(it + PCMITEM_SIZE,       ADEC_PCM_BYTES);
        /* bsiInfo must point at a codec info block, not be null. Resistance:
         * Fall of Man sizes each AC3 block from bsiInfo+0x14; with a null
         * pointer that read address 0x14 and came back 0, so its ring buffer
         * never filled, it never started its audio port, and its movie player
         * waited forever. ponytail: only that output-size word is filled; the
         * rest of the block is zero until a title reads another field. */
        const u32 bsi = ADEC_BSI_EA(handle);
        for (u32 o = 0; o < ADEC_BSI_BYTES; o += 4) vm_write32(bsi + o, 0);
        vm_write32(bsi + 0x14, ADEC_PCM_BYTES);
        vm_write32(it + PCMITEM_BSI_INFO,   bsi);
        /* Echo the AU back, which is what a real decoder does -- the guest
         * matches returned PCM against the AU it submitted. */
        vm_write32(it + PCMITEM_AU_START,
                   vm_read32(au + (u32)offsetof(CellAdecAuInfo, startAddr)));
        vm_write32(it + PCMITEM_AU_SIZE,
                   vm_read32(au + (u32)offsetof(CellAdecAuInfo, size)));
        vm_write32(it + PCMITEM_AU_PTS_HI,       vm_read32(au + 0x08));
        vm_write32(it + PCMITEM_AU_PTS_LO,       vm_read32(au + 0x0C));
        vm_write32(it + PCMITEM_AU_USERDATA,     vm_read32(au + 0x10));
        vm_write32(it + PCMITEM_AU_USERDATA + 4, vm_read32(au + 0x14));
    }
    a->hasPcm = 1;

    adec_notify(handle, CELL_ADEC_MSG_TYPE_PCMOUT, CELL_OK);

    return CELL_OK;
}

s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer)
{
    { static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 6)
        printf("[cellAdec] GetPcm(handle=%u out=0x%08X)\n",
               handle, GUEST_EA(outBuffer)); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    /* outBuffer is the GUEST's buffer. Leaving it untouched hands the title
     * whatever happened to be there, which it then plays. */
    { const u32 out = GUEST_EA(outBuffer);
      if (out) for (u32 o = 0; o < ADEC_PCM_BYTES; o += 4) vm_write32(out + o, 0); }

    s_adec[handle].hasPcm = 0;
    return CELL_OK;
}

s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem)
{
    { static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 6)
        printf("[cellAdec] GetPcmItem(handle=%u out=0x%08X hasPcm=%d)\n",
               handle, GUEST_EA(pcmItem),
               handle < MAX_ADEC ? s_adec[handle].hasPcm : -1); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    /* pcmItem is a GUEST CellAdecPcmItem**. Writing &host_struct here put a
     * HOST address into guest memory; the guest dereferenced it and died on a
     * wild address (rip 0x870D0E, fault 0xCFFDFFF0). Write the guest EA. */
    if (pcmItem)
        vm_write32(GUEST_EA(pcmItem), s_adec[handle].itemEa);

    return CELL_OK;
}
