/*
 * Unit test for sys_process_is_spu_lock_line_reservation_address (14).
 *
 * The contract is RPCS3's process_is_spu_lock_line_reservation_address
 * (Emu/Cell/lv2/sys_process.cpp): the main-memory (0x0/0x1), user-64K (0x2),
 * RSX-local (0xC) and RawSPU-MMIO (0xE) segments are reservation-capable;
 * private SPU MMIO (0xF) refuses RawSPU with EPERM; the PPU stack segment
 * (0xD) and sys_vm mappings refuse with EPERM; holes in a mapped sys_vm
 * region and unmapped addresses answer EINVAL; and the flags must be
 * non-zero and contain only the two SPU bits.
 *
 * Stands alone: links only lv2_lock_line.c and defines the sys_vm mapping
 * table itself (the real one lives in sys_vm.c).
 */
#include "lv2_lock_line.h"
#include "sys_vm.h"
#include "ps3emu/error_codes.h"

#include <stdio.h>

/* The test owns the sys_vm mapping table; the real definition is sys_vm.c. */
sys_vm_map_info g_sys_vm_maps[SYS_VM_MAX];

static int g_fail = 0;

static void expect(const char* what, uint32_t addr, uint64_t flags, int64_t want)
{
    const int64_t got = lv2_spu_lock_line_classify(addr, flags);
    if (got != want) {
        printf("FAIL %s: classify(0x%08X, flags=0x%llX) = %lld, want %lld\n",
               what, addr, (unsigned long long)flags,
               (long long)got, (long long)want);
        g_fail++;
    }
}

int main(void)
{
    const int64_t OK     = 0;
    const int64_t EPERM  = (int64_t)(int32_t)CELL_EPERM;
    const int64_t EINVAL = (int64_t)(int32_t)CELL_EINVAL;

    /* Reservation-capable segments (0x0/0x1/0x2/0xC/0xE). */
    expect("main low",      0x00010000u, 0x2, OK);
    expect("main top",      0x0FFFFFFFu, 0x2, OK);
    expect("segment 1 low", 0x10000000u, 0x2, OK);
    expect("segment 1 mid", 0x11000000u, 0x2, OK);  /* beyond the old 256 MB window */
    expect("segment 1 top", 0x1FFFFFFFu, 0x2, OK);
    expect("user 64K low",  0x20000000u, 0x2, OK);
    expect("user 64K top",  0x2FFFFFFFu, 0x2, OK);
    expect("rsx local",     0xC0000000u, 0x2, OK);
    expect("rawspu mmio",   0xE0000000u, 0x2, OK);

    /* The same addresses with the RawSPU flag. */
    expect("segment 1 rawspu", 0x11000000u, 0x1, OK);

    /* Segment 0xF: private SPU MMIO answers EPERM only for RawSPU. */
    expect("priv spu rawspu", 0xF0000000u, 0x1, EPERM);
    expect("priv spu thr",    0xF0000000u, 0x2, OK);

    /* Segment 0xD: the PPU stack area refuses both access kinds. */
    expect("ppu stack thr",   0xD0000000u, 0x2, EPERM);
    expect("ppu stack rawspu", 0xD0000000u, 0x1, EPERM);

    /* sys_vm: a covered address answers EPERM, a hole in the mapped region
     * EINVAL, and the empty region EINVAL (nothing mapped). */
    g_sys_vm_maps[0].active = 1;
    g_sys_vm_maps[0].addr   = 0x60000000u;
    g_sys_vm_maps[0].vsize  = 0x02000000u;   /* 32 MB */
    g_sys_vm_maps[0].psize  = 0x00010000u;
    expect("sys_vm start",  0x60000000u, 0x2, EPERM);
    expect("sys_vm end",    0x61FFFFFFu, 0x2, EPERM);
    expect("sys_vm hole",   0x62000000u, 0x2, EINVAL);
    g_sys_vm_maps[0].active = 0;
    expect("sys_vm empty",  0x60000000u, 0x2, EINVAL);

    /* Outside every known window: EINVAL. */
    expect("unmapped 0x3", 0x30000000u, 0x2, EINVAL);
    expect("unmapped 0x7", 0x70000000u, 0x2, EINVAL);
    expect("unmapped 0xB", 0xB0000000u, 0x2, EINVAL);

    /* Flags: zero, or anything beyond the two SPU bits, is EINVAL; both
     * bits together is a valid mask. */
    expect("flags zero",  0x11000000u, 0x0, EINVAL);
    expect("flags both",  0x11000000u, 0x3, OK);
    expect("flags bit 2", 0x11000000u, 0x4, EINVAL);
    expect("flags high",  0x11000000u, 0x80010009ull, EINVAL);

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_lock_line: all checks passed\n");
    return 0;
}
