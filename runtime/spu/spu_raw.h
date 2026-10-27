/* spu_raw.h — raw SPU MMIO window (0xE0000000) and the lv2 sys_raw_spu_* facility.
 *
 * A RAW SPU is not a SPU thread. lv2 hands the process a physical SPU and maps its
 * local store and problem-state registers straight into the address space; the PPU
 * then drives it by ordinary loads and stores, with no kernel in the path:
 *
 *   0xE0000000 + n*0x100000 + 0x00000   local store        (256 KB)
 *   0xE0000000 + n*0x100000 + 0x40000   problem state      (registers below)
 *
 * PS3 firmware modules use raw SPUs rather than SPURS -- ps1_netemu runs its
 * GPU/R3000 cores this way (cell/xspu.cc: sys_raw_spu_create). The SPURS path in
 * spu_workload.c cannot serve them: there is no workload image handed to a kernel
 * to fingerprint, no job descriptor, and no dispatch call to intercept. The SPU
 * simply starts when someone writes 1 to its run-control register.
 *
 * Local store is REAL GUEST MEMORY here, not a copy. The PPU writes the SPU's code
 * and its command buffers straight into the window while the SPU is running, so a
 * private 256 KB buffer with copy-in/copy-out would race every frame. The raw SPU's
 * spu_context therefore points its `ls` at vm_base + the window (which is why
 * spu_context.ls became a pointer). Endianness needs no care: LS is raw bytes and
 * both sides are big-endian.
 */
#ifndef SPU_RAW_H
#define SPU_RAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_RAW_BASE      0xE0000000u
#define SPU_RAW_STRIDE    0x00100000u
#define SPU_RAW_COUNT     6u                    /* lv2 exposes 5; one spare */
#define SPU_RAW_SPAN      (SPU_RAW_COUNT * SPU_RAW_STRIDE)
#define SPU_RAW_PROB_OFF  0x40000u              /* LS below this, registers above */

/* Problem-state register offsets, relative to the SPU's 1 MB base. */
#define SPU_RAW_MFC_LSA        0x43004u
#define SPU_RAW_MFC_EAH        0x43008u
#define SPU_RAW_MFC_EAL        0x4300Cu
#define SPU_RAW_MFC_SIZE_TAG   0x43010u
#define SPU_RAW_MFC_CLASS_CMD  0x43014u
#define SPU_RAW_MFC_QSTATUS    0x43104u
#define SPU_RAW_PRXY_QUERYTYPE 0x43204u
#define SPU_RAW_PRXY_QUERYMASK 0x4321Cu
#define SPU_RAW_PRXY_TAGSTATUS 0x4322Cu
#define SPU_RAW_OUT_MBOX       0x44004u
#define SPU_RAW_IN_MBOX        0x4400Cu
#define SPU_RAW_MBOX_STATUS    0x44014u
#define SPU_RAW_RUNCNTL        0x4401Cu
#define SPU_RAW_STATUS         0x44024u
#define SPU_RAW_NPC            0x44034u
#define SPU_RAW_SIG_NOTIFY1    0x5400Cu
#define SPU_RAW_SIG_NOTIFY2    0x5C00Cu

/* True for an address inside a raw SPU's PROBLEM STATE. Deliberately excludes
 * local store, which is plain memory and must stay on the fast path -- LS traffic
 * is the bulk of what the PPU writes into this window. Two ops, no branch on the
 * common case. */
static inline int spu_raw_is_reg(uint32_t ea)
{
    uint32_t off = ea - SPU_RAW_BASE;
    return off < SPU_RAW_SPAN && (off & (SPU_RAW_STRIDE - 1)) >= SPU_RAW_PROB_OFF;
}

/* Store side effects (run control, mailboxes, proxy DMA). The plain memory store
 * still happens in vm_write*; this runs after it. */
void spu_raw_reg_store(uint32_t ea, uint32_t val, int width);

/* Load side effects. Reading the outbound mailbox POPS it, so a read cannot be
 * served from memory alone. Returns 1 and sets *out when it owns the value;
 * 0 leaves the caller's plain memory read alone. */
int  spu_raw_reg_load(uint32_t ea, uint32_t* out);

/* lv2 registration (runtime/syscalls/lv2_register.c). */
struct lv2_syscall_table;
void sys_raw_spu_init(struct lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SPU_RAW_H */
