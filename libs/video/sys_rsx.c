/*
 * ps3recomp - sys_rsx_* : the lv2 RSX syscalls
 *
 * WHY THIS EXISTS
 *
 * Every ps3recomp port so far reaches RSX through the HLE cellGcmSys imports:
 * the title calls cellGcmSetDrawArrays & friends, libs/video/cellGcmSys.c owns
 * the FIFO, its walker decodes the method stream and feeds rsx_live_draw. That
 * only works for a title that *imports* cellGcmSys.
 *
 * A PS3 FIRMWARE module does not. /dev_flash/ps1emu/ps1_netemu.self (the PS1
 * emulator PSOne Classics run under) links libgcm statically -- the library's
 * own debug assertions are still in the image -- and talks to the GPU through
 * the kernel. No import to hook, so the whole HLE layer is bypassed and the
 * live draw engine has nothing to walk.
 *
 * This file is the missing bottom half. It is a thin bridge, not a second RSX:
 * every handler routes into the state cellGcmSys.c already keeps, so the FIFO
 * walker, the NV4097 method decoder, the D3D12 backend and RSX_LIVE_DRAW=1 are
 * all unchanged. The tap point moves down a layer; nothing else moves.
 *
 * THE CONTRACT, AND WHERE IT COMES FROM
 *
 * Derived from ps1_netemu's own .text rather than from documentation, by
 * disassembling each `li r11,N` / `sc` pair and the libgcm wrapper around it.
 * The numbering it uses is:
 *
 *   666 device_open      670 context_allocate   674 context_attribute
 *   667 device_close     671 context_free       675 device_map
 *   668 memory_allocate  672 context_iomap      676 device_unmap
 *   669 memory_free      673 context_iounmap    677 attribute
 *
 * Signatures confirmed against the call sites (wrapper prologue -> syscall arg
 * setup), which is also where the two facts that make this work came from:
 *
 *   - cellGcmInit's real failure was a VERSION CHECK, not a missing syscall:
 *       ld r7,0x88(r1) / lwz r0,0x0(r9) / cmpwi r0,529
 *     i.e. *(u32*)lpar_driver_info must equal 0x211.
 *
 *   - the put/get/ref triple lives at lpar_dma_control + 0x40:
 *       lwz r3,0x18(r9) / addi r3,r3,64
 *     so context_allocate returns (control EA - 0x40) and the driver's own
 *     flushes land exactly where the existing walker reads `put`.
 *
 * Packet ids for context_attribute are likewise only the ones this image
 * actually issues (r4 at each of its 23 call sites): 0x001 0x002 0x003 0x101
 * 0x104 0x106 0x108 0x10A 0x202 0x300 0x301 0x302. The ones that matter for
 * pixels are 0x001 (FIFO) and 0x104 (display buffer); the rest are accepted and
 * logged once each rather than guessed at, because a wrong guess here writes
 * plausible garbage into driver state instead of failing loudly.
 */

#include "../../runtime/syscalls/lv2_syscall_table.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm.h"          /* VM_HLE_INJECT_BASE */
#include "../../include/ps3emu/error_codes.h"
#include "cellGcmSys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/syscalls/sys_event.h"

/* RsxDriverInfo fields libgcm's interrupt thread depends on. Offsets confirmed
 * against RPCS3's struct RsxDriverInfo (Emu/Cell/lv2/sys_rsx.h).
 *
 * _gcm_intr_thread (OPD 0x1B7768 -> code 0x1A584 in ps1_netemu) does, at
 * 0x1A628:  lwz r3, 0x12D0(r9) ; sc  -- 130 = sys_event_queue_receive. So
 * +0x12D0 IS the queue id it blocks on. Left zero, the thread received on queue
 * 0, returned at once and EXITED; nothing then ran gcm's flip handler, so
 * ps1_netemu's own flip wait at 0x113EB0 (a sys_semaphore_wait that only that
 * handler posts) never woke. That parked the MAIN guest thread -- the one
 * running the R3000 -- so the PS1 core stopped after ~42,000 instructions. */
#define RSX_DI_HANDLERS         0x12C0u   /* mask: which events gcm wants */
#define RSX_DI_HANDLER_QUEUE    0x12D0u   /* event queue id for the ISR   */
#define RSX_DI_USER_CMD_PARAM   0x12CCu   /* arg the USER_CMD handler reads */
#define RSX_DI_HEAD_BASE        0x10B8u   /* head[8], stride 0x40         */
#define RSX_DI_HEAD_STRIDE      0x40u
#define RSX_DI_HEAD_FLIPFLAGS   0x08u
#define RSX_DI_HEAD_LASTQUEUED  0x14u

#define SYS_RSX_EVENT_VBLANK      (1u << 1)
#define SYS_RSX_EVENT_FLIP_BASE   (1u << 3)
#define SYS_RSX_EVENT_QUEUE_BASE  (1u << 5)
#define SYS_RSX_EVENT_USER_CMD    (1u << 7)

/* What ps1_netemu actually registers. Its ISR is a bit -> handler-slot table
 * (handler object = *(TOC-0x6AB4)), read straight out of the image:
 *
 *     0x0001 -> +0x0C   0x0002 -> +0x10   0x0004 -> +0x04
 *     0x0020 -> +0x24   0x0040 -> +0x28   0x0080 -> +0x2C
 *     0x0400 -> +0x14   0x0800 -> +0x18
 *
 * and the mask it publishes at +0x12C0 settles to 0x84. So the only two
 * handlers this firmware ever installs are 0x80 and 0x04 -- and 0x04 is its
 * GRAPHICS ERROR handler, not a flip handler: driving it made the guest print
 *
 *     [RSX dump analysis] unsupported error
 *     graphics error 0 : 00000000 ...
 *
 * and dump its whole RSX state, 60 times a second. It registers NO vblank and
 * NO flip handler, so there is nothing here for a timer to send legitimately,
 * and the ticker that used to live here has been removed. Publishing the queue
 * is still both necessary and correct: without it the ISR thread received on
 * queue 0 and exited outright, and it must instead sit blocked waiting for real
 * events. rsx_send_event stays for whoever wires those up (a genuine flip or
 * user command), with the handler-mask filter that makes it safe. */

/* Our own ipc key for the ISR queue. The guest never looks it up by key -- it
 * only reads the id we publish at +0x12D0. */
#define RSX_ISR_QUEUE_KEY  0x8000000000005250ull


/* ---------------------------------------------------------------------------
 * Guest memory the kernel hands the driver.
 *
 * Placed in the HLE inject window, clear of everything cellGcmSys.c already
 * puts there (labels +0x0000, control +0x2000, callback +0x2F00, the two offset
 * tables +0x3000/+0x5000). The window is at least 16 MB (rsx_commands.c indexes
 * it with a 24-bit offset), so +0x30000 onward is ours.
 * -----------------------------------------------------------------------*/
#define RSX_DEVICE_EA       (VM_HLE_INJECT_BASE + 0x30000u)   /* dev_id 8 page */
#define RSX_DRIVER_INFO_EA  (VM_HLE_INJECT_BASE + 0x31000u)   /* RsxDriverInfo  */
#define RSX_REPORTS_EA      (VM_HLE_INJECT_BASE + 0x38000u)   /* report/label   */
#define RSX_DRIVER_INFO_SZ  0x7000u
#define RSX_REPORTS_SZ      0x8000u

#define RSX_CONTEXT_ID      0x55555555u
#define RSX_MEM_HANDLE      0x5A5A0001u

/* The driver's version handshake. ps1_netemu's cellGcmInit compares
 * *(u32*)lpar_driver_info against 529 and gives up if it differs, which is what
 * "[GPU] cellGcmInit failed" was. */
#define RSX_DRIVER_VERSION  0x211u
#define RSX_GPU_VERSION     0x5Cu

static int s_rsx_dbg = -1;
static int rsx_dbg(void)
{
    if (s_rsx_dbg < 0) s_rsx_dbg = getenv("SYS_RSX_DBG") ? 1 : 0;
    return s_rsx_dbg;
}

/* ---------------------------------------------------------------------------
 * Device / driver-info pages
 * -----------------------------------------------------------------------*/

/* Idempotent: context_allocate also calls this in case memory_allocate never ran,
 * and re-running it must not wipe the real local size back to a placeholder. */
static int s_di_ready = 0;

static void rsx_isr_queue_bringup(void);

static void rsx_driver_info_init(u32 local_size)
{
    if (s_di_ready) return;
    s_di_ready = 1;
    for (u32 o = 0; o < RSX_DRIVER_INFO_SZ; o += 4)
        vm_write32(RSX_DRIVER_INFO_EA + o, 0);
    for (u32 o = 0; o < RSX_REPORTS_SZ; o += 4)
        vm_write32(RSX_REPORTS_EA + o, 0);

    /* Only the fields whose offsets are confirmed. The version at +0 is read by
     * the driver (see the header comment); the rest are the identity/clock
     * fields libgcm reports back through cellGcmGetConfiguration, which are
     * cosmetic here but wrong-looking if left zero. Anything this image reads
     * that is still zero will show up as a specific failure to chase, which is
     * a better outcome than a guessed layout that silently half-works. */
    vm_write32(RSX_DRIVER_INFO_EA + 0x00, RSX_DRIVER_VERSION);
    vm_write32(RSX_DRIVER_INFO_EA + 0x04, RSX_GPU_VERSION);
    vm_write32(RSX_DRIVER_INFO_EA + 0x08, local_size);
    vm_write32(RSX_DRIVER_INFO_EA + 0x0C, 0);              /* hardware channel */
    vm_write32(RSX_DRIVER_INFO_EA + 0x10, 500000000u);     /* nvcore frequency */
    vm_write32(RSX_DRIVER_INFO_EA + 0x14, 650000000u);     /* memory frequency */

    for (u32 o = 0; o < 0x1000u; o += 4)
        vm_write32(RSX_DEVICE_EA + o, 0);

    rsx_isr_queue_bringup();
}

/* ---------------------------------------------------------------------------
 * The gcm interrupt queue and its vblank tick
 * -----------------------------------------------------------------------*/

static uint32_t s_isr_qid = 0;

/* Push one RSX event, filtered by the handler mask gcm publishes at +0x12C0,
 * exactly as rsx::thread::send_event does. Sending an unmasked event is not
 * merely wasteful: gcm's ISR dispatches on the flag bits, so a bit it never
 * asked for reaches a handler slot it never filled in. */
static void rsx_send_event(uint64_t flags)
{
    if (!s_isr_qid) return;
    uint32_t mask = vm_read32(RSX_DRIVER_INFO_EA + RSX_DI_HANDLERS);
    flags &= (uint64_t)mask;
    if (!flags) return;
    sys_event_queue_push_by_id(s_isr_qid, 0, 0, flags, 0);
}

/* The RSX raises this when the FIFO executes GCM_SET_USER_COMMAND (method
 * 0xEB00): lv2 stashes the method argument where the handler will read it, then
 * posts the event -- exactly what sys_rsx_context_attribute's 0xFEF case does.
 *
 * This is the "user command" the note above left for whoever wired it up. It is
 * not optional for this title: the mask settles to 0x84, and the 0x80 handler
 * ps1_netemu installs is the one that posts the semaphore its flip path blocks
 * on. A dropped 0xEB00 therefore parks the emulator permanently.
 *
 * Called from the FIFO walker in cellGcmSys.c. */
void rsx_raise_user_cmd(uint32_t arg)
{
    { static unsigned long n = 0;
      /* Periodic, not just the first four: the question that matters is
       * whether these STOP. A freeze parks the R3000 waiting on spu4, spu4
       * waiting for SigNotify2 from the thread that services this event, so
       * "did the last user command arrive before the freeze" is the join
       * between the two halves of the deadlock. */
      if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 4 || (n % 256) == 0) {
          uint32_t mask = vm_read32(RSX_DRIVER_INFO_EA + RSX_DI_HANDLERS);
          fprintf(stderr, "[usercmd] #%lu arg=0x%08X handlers=0x%X qid=%u\n",
                  n, arg, mask, s_isr_qid); fflush(stderr); } }
    vm_write32(RSX_DRIVER_INFO_EA + RSX_DI_USER_CMD_PARAM, arg);
    rsx_send_event(SYS_RSX_EVENT_USER_CMD);
}

static void rsx_isr_queue_bringup(void)
{
    if (s_isr_qid) return;
    s_isr_qid = sys_event_queue_create_direct(RSX_ISR_QUEUE_KEY, 0x20);
    if (!s_isr_qid) {
        fprintf(stderr, "[sys_rsx] could not create the gcm ISR event queue\n");
        return;
    }
    vm_write32(RSX_DRIVER_INFO_EA + RSX_DI_HANDLER_QUEUE, s_isr_qid);
    fprintf(stderr, "[sys_rsx] gcm ISR queue id=%u published at driver_info+0x12D0\n",
            s_isr_qid);
}

/* ---------------------------------------------------------------------------
 * Handlers
 * -----------------------------------------------------------------------*/

static int64_t sc_rsx_device_open(ppu_context* ctx)  { (void)ctx; return CELL_OK; }
static int64_t sc_rsx_device_close(ppu_context* ctx) { (void)ctx; return CELL_OK; }

/* 668: sys_rsx_memory_allocate(u32* handle, u64* addr, u32 size, u32 flags,
 *                              u64 a5, u64 a6, u64 a7)
 * Hands back the base of RSX local memory. cellGcmSys.c owns that address (its
 * cellGcmInit picks it and every local-memory offset resolves against it), so
 * take it from there rather than inventing a second one. */
static int64_t sc_rsx_memory_allocate(ppu_context* ctx)
{
    u32 h_ea    = (u32)ctx->gpr[3];
    u32 addr_ea = (u32)ctx->gpr[4];
    u32 size    = (u32)ctx->gpr[5];

    u32 local = cellGcm_syscall_bringup(size);
    rsx_driver_info_init(size);

    if (h_ea)    vm_write32(h_ea, RSX_MEM_HANDLE);
    if (addr_ea) vm_write64(addr_ea, (uint64_t)local);

    if (rsx_dbg())
        fprintf(stderr, "[sys_rsx] memory_allocate(size=0x%X flags=0x%X) -> "
                        "local=0x%08X handle=0x%08X\n",
                size, (u32)ctx->gpr[6], local, RSX_MEM_HANDLE);
    return CELL_OK;
}

static int64_t sc_rsx_memory_free(ppu_context* ctx) { (void)ctx; return CELL_OK; }

/* 670: sys_rsx_context_allocate(u32* ctx_id, u64* lpar_dma_control,
 *                               u64* lpar_driver_info, u64* lpar_reports,
 *                               u64 mem_handle, u64 system_mode) */
static int64_t sc_rsx_context_allocate(ppu_context* ctx)
{
    u32 id_ea  = (u32)ctx->gpr[3];
    u32 dma_ea = (u32)ctx->gpr[4];
    u32 di_ea  = (u32)ctx->gpr[5];
    u32 rep_ea = (u32)ctx->gpr[6];

    /* memory_allocate always runs first on this path, but do not depend on it. */
    cellGcm_syscall_bringup(0);
    rsx_driver_info_init(0x10000000u);   /* no-op if memory_allocate already ran */

    /* The driver reads put/get/ref at lpar_dma_control + 0x40, and the FIFO
     * walker reads them at cellGcm_control_guest_addr(). Line the two up here
     * and the driver's ordinary flushes drive the existing walker with no
     * further plumbing. SYS_RSX_DMACTL_OFF overrides the 0x40 if some other
     * firmware revision disagrees. */
    static int off = -1;
    if (off < 0) { const char* e = getenv("SYS_RSX_DMACTL_OFF");
                   off = e ? (int)strtol(e, NULL, 0) : 0x40; }
    u32 dma_control = cellGcm_control_guest_addr() - (u32)off;

    if (id_ea)  vm_write32(id_ea, RSX_CONTEXT_ID);
    if (dma_ea) vm_write64(dma_ea, (uint64_t)dma_control);
    if (di_ea)  vm_write64(di_ea,  (uint64_t)RSX_DRIVER_INFO_EA);
    if (rep_ea) vm_write64(rep_ea, (uint64_t)RSX_REPORTS_EA);

    fprintf(stderr, "[sys_rsx] context_allocate -> id=0x%08X dma_control=0x%08X "
                    "(ctrl=0x%08X) driver_info=0x%08X reports=0x%08X mode=0x%llX\n",
            RSX_CONTEXT_ID, dma_control, cellGcm_control_guest_addr(),
            RSX_DRIVER_INFO_EA, RSX_REPORTS_EA,
            (unsigned long long)ctx->gpr[8]);
    return CELL_OK;
}

static int64_t sc_rsx_context_free(ppu_context* ctx) { (void)ctx; return CELL_OK; }

/* 672: sys_rsx_context_iomap(u32 ctx, u32 io, u32 ea, u32 size, u64 flags)
 * Arg order taken from the wrapper: cellGcmMapEaIoAddress(ea, io, size) loads
 * r30=r28=ea, r29=io, r31=size and passes r4=r29(io), r5=r28(ea), r6=r31. */
static int64_t sc_rsx_context_iomap(ppu_context* ctx)
{
    u32 io   = (u32)ctx->gpr[4];
    u32 ea   = (u32)ctx->gpr[5];
    u32 size = (u32)ctx->gpr[6];

    cellGcm_syscall_iomap(ea, io, size);
    if (rsx_dbg())
        fprintf(stderr, "[sys_rsx] iomap io=0x%08X <- ea=0x%08X size=0x%X\n",
                io, ea, size);
    return CELL_OK;
}

/* 673: sys_rsx_context_iounmap(u32 ctx, u32 io, u32 size) */
static int64_t sc_rsx_context_iounmap(ppu_context* ctx)
{
    cellGcm_syscall_iounmap((u32)ctx->gpr[4], (u32)ctx->gpr[5]);
    return CELL_OK;
}

/* 674: sys_rsx_context_attribute(u32 ctx, u32 packet_id, u64 a3..a6) */
static int64_t sc_rsx_context_attribute(ppu_context* ctx)
{
    u32 pkt = (u32)ctx->gpr[4];
    uint64_t a3 = ctx->gpr[5], a4 = ctx->gpr[6], a5 = ctx->gpr[7];

    switch (pkt) {
    case 0x001:                       /* FIFO: a3 = put, a4 = get (IO offsets) */
        cellGcm_syscall_set_fifo((u32)a3, (u32)a4);
        if (rsx_dbg())
            fprintf(stderr, "[sys_rsx] FIFO put=0x%08X get=0x%08X\n",
                    (u32)a3, (u32)a4);
        return CELL_OK;

    case 0x104: {                     /* display buffer set */
        /* Packing read off cellGcmSetDisplayBuffer(id, offset, pitch, w, h):
         *   a3 = id & 0xFF
         *   a4 = (width << 32) | height
         *   a5 = (pitch << 32) | offset                                     */
        u32 id     = (u32)(a3 & 0xFFu);
        u32 width  = (u32)(a4 >> 32), height = (u32)a4;
        u32 pitch  = (u32)(a5 >> 32), offset = (u32)a5;
        cellGcmSetDisplayBuffer(id, offset, pitch, width, height);
        return CELL_OK;
    }

    default: {
        /* Accepted, not emulated. Once per packet id so a title that issues one
         * every frame does not drown the log, but the set it uses is visible. */
        static u32 seen[32]; static int n = 0;
        int f = 0;
        for (int i = 0; i < n; i++) if (seen[i] == pkt) f = 1;
        if (!f && n < 32) {
            seen[n++] = pkt;
            fprintf(stderr, "[sys_rsx] context_attribute packet 0x%03X accepted "
                            "(not emulated) a3=0x%llX a4=0x%llX a5=0x%llX\n",
                    pkt, (unsigned long long)a3, (unsigned long long)a4,
                    (unsigned long long)a5);
        }
        return CELL_OK;
    }
    }
}

/* 675: sys_rsx_device_map(u64* out_addr, u64* out_a2, u64 dev_id)
 * dev_id 8 is the GPU. The image also probes dev_id 9 (map then immediately
 * unmap) and copes with a failure, so report only the device we have. */
static int64_t sc_rsx_device_map(ppu_context* ctx)
{
    u32 a_ea = (u32)ctx->gpr[3], b_ea = (u32)ctx->gpr[4];
    uint64_t dev = ctx->gpr[5];

    if (dev != 8) {
        if (rsx_dbg())
            fprintf(stderr, "[sys_rsx] device_map(dev=%llu) -> not present\n",
                    (unsigned long long)dev);
        return CELL_EINVAL;
    }
    if (a_ea) vm_write64(a_ea, (uint64_t)RSX_DEVICE_EA);
    if (b_ea) vm_write64(b_ea, 0);
    if (rsx_dbg())
        fprintf(stderr, "[sys_rsx] device_map(dev=8) -> 0x%08X\n", RSX_DEVICE_EA);
    return CELL_OK;
}

static int64_t sc_rsx_device_unmap(ppu_context* ctx) { (void)ctx; return CELL_OK; }
static int64_t sc_rsx_attribute(ppu_context* ctx)    { (void)ctx; return CELL_OK; }

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_rsx_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, 666, sc_rsx_device_open);
    lv2_syscall_register(tbl, 667, sc_rsx_device_close);
    lv2_syscall_register(tbl, 668, sc_rsx_memory_allocate);
    lv2_syscall_register(tbl, 669, sc_rsx_memory_free);
    lv2_syscall_register(tbl, 670, sc_rsx_context_allocate);
    lv2_syscall_register(tbl, 671, sc_rsx_context_free);
    lv2_syscall_register(tbl, 672, sc_rsx_context_iomap);
    lv2_syscall_register(tbl, 673, sc_rsx_context_iounmap);
    lv2_syscall_register(tbl, 674, sc_rsx_context_attribute);
    lv2_syscall_register(tbl, 675, sc_rsx_device_map);
    lv2_syscall_register(tbl, 676, sc_rsx_device_unmap);
    lv2_syscall_register(tbl, 677, sc_rsx_attribute);
}
