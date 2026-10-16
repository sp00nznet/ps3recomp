/*
 * ps3recomp - sys_process_is_spu_lock_line_reservation_address (14)
 *
 * Classification for syscall 14. Split out of lv2_register.c so the
 * contract can be unit-tested without linking the whole syscall layer.
 *
 * Contract from RPCS3's sys_process.cpp
 * (process_is_spu_lock_line_reservation_address):
 *  - the flags must be non-zero and contain only the two SPU bits
 *    (SPU_THR 0x2, RAW_SPU 0x1) or EINVAL -- the mask is a set of access
 *    rights, so both bits together is a valid request;
 *  - the main-memory (0x0/0x1), user-64K/sys_memory (0x2), RSX-local (0xC)
 *    and RawSPU-MMIO (0xE) segments are reservation-capable;
 *  - private SPU MMIO (0xF) refuses RawSPU access with EPERM, and allows
 *    SPU threads;
 *  - the PPU stack segment (0xD) refuses with EPERM;
 *  - everything else: a sys_vm mapping answers EPERM, an uncovered hole in
 *    a sys_vm-mapped 256 MB region answers EINVAL (the mapping reserves
 *    the whole region), and anything unmapped answers EINVAL.
 */

#include "lv2_lock_line.h"

#include "sys_vm.h"                       /* SYS_VM_REGION_*, g_sys_vm_maps */

#include "../../include/ps3emu/error_codes.h"

int64_t lv2_spu_lock_line_classify(uint32_t addr, uint64_t flags)
{
    if (!flags || (flags & ~0x3ull)) {        /* SPU_THR 0x2 | RAW_SPU 0x1 */
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    switch (addr >> 28) {
    case 0x0:                                 /* main memory */
    case 0x1:                                 /* main memory (the 256 MB
                                                 VM_MAIN window is only the
                                                 bottom of the segment) */
    case 0x2:                                 /* user 64K (sys_memory) */
    case 0xC:                                 /* RSX local memory */
    case 0xE:                                 /* RawSPU MMIO */
        return 0;

    case 0xF:                                 /* private SPU MMIO */
        if (flags & 0x1ull) {
            /* RawSPU may not reserve lock lines on its own MMIO */
            return (int64_t)(int32_t)CELL_EPERM;
        }
        return 0;

    case 0xD:                                 /* PPU stack area */
        return (int64_t)(int32_t)CELL_EPERM;

    default:
        /* sys_vm mappings live in the one 256 MB window sys_vm.h tracks.
         * RPCS3 answers EPERM for an address a mapping covers, EINVAL for
         * a hole in that region (a mapping reserves all of it), and EINVAL
         * for anything else, since no other mapping lives in these
         * segments. */
        if (addr >= SYS_VM_REGION_BASE && addr < SYS_VM_REGION_END) {
            int mapped = 0;
            for (int i = 0; i < SYS_VM_MAX; i++) {
                const sys_vm_map_info* m = &g_sys_vm_maps[i];
                if (!m->active) {
                    continue;
                }
                mapped = 1;
                if (addr >= m->addr && addr - m->addr < m->vsize) {
                    return (int64_t)(int32_t)CELL_EPERM;
                }
            }
            if (mapped) {
                return (int64_t)(int32_t)CELL_EINVAL;   /* hole in the region */
            }
        }
        return (int64_t)(int32_t)CELL_EINVAL;           /* unmapped */
    }
}
