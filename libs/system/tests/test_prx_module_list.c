/*
 * sys_prx_get_module_list / sys_prx_get_module_info.
 *
 * The module list has no PRX table behind it yet, but it must still write
 * count = 0 back to the caller. Leaving the out-param untouched is what made
 * a title walk its own initializer (max/count = 50) as a module id list and
 * call sys_prx_get_module_info with stack addresses.
 *
 * Build (also wired as the prx_module_list CTest):
 *   cc -std=gnu17 -I . -I include -I runtime/ppu -I runtime/memory \
 *      -I runtime/platform -I runtime/syscalls \
 *      -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *      libs/system/tests/test_prx_module_list.c libs/system/sysPrxForUser.c
 */
#include "libs/system/sysPrxForUser.h"
#include "ppu_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t* vm_base;

/* Host-side hooks the ppu_memory store path references; the test never arms
 * any of them, so they are no-ops (same stubs as test_sysconf.c). */
uint32_t g_ww_lo, g_ww_hi;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static int fails;

#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s\n", msg); fails++; } \
} while (0)

int main(void)
{
    const uint32_t info_ea  = 0x1000;
    const uint32_t small_ea = 0x1100;
    const uint32_t idlist_ea = 0x2000;
    s32 rc;

    vm_base = calloc(1, 0x10000);
    if (!vm_base) { perror("calloc"); return 1; }

    /* Shape the title passes: size, max, its own count, and an id-list EA. */
    vm_write64(info_ea + 0x00, 0x18);
    vm_write32(info_ea + 0x08, 50);
    vm_write32(info_ea + 0x0C, 50);
    vm_write32(info_ea + 0x10, idlist_ea);

    rc = sys_prx_get_module_list(0, (void*)(uintptr_t)info_ea);
    CHECK(rc == CELL_OK, "get_module_list returns CELL_OK");
    CHECK(vm_read64(info_ea + 0x00) == 0x18, "get_module_list leaves size alone");
    CHECK(vm_read32(info_ea + 0x08) == 50, "get_module_list leaves max alone");
    CHECK(vm_read32(info_ea + 0x0C) == 0, "get_module_list writes count = 0");
    CHECK(vm_read32(info_ea + 0x10) == idlist_ea, "get_module_list leaves idlist alone");

    rc = sys_prx_get_module_list(0, NULL);
    CHECK(rc != CELL_OK, "get_module_list rejects a null info pointer");

    vm_write64(small_ea + 0x00, 0x10);
    rc = sys_prx_get_module_list(0, (void*)(uintptr_t)small_ea);
    CHECK(rc != CELL_OK, "get_module_list rejects an undersized info struct");

    rc = sys_prx_get_module_info(0x1234, 0, (void*)(uintptr_t)info_ea);
    CHECK((uint32_t)rc == 0x8001112Eu,
          "get_module_info returns CELL_PRX_ERROR_UNKNOWN_MODULE");

    rc = sys_prx_get_module_info(0, 0, NULL);
    CHECK(rc != CELL_OK, "get_module_info rejects a null info pointer");

    free(vm_base);
    printf("Results: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
