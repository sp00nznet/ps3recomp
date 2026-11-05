/* macOS: clang -std=gnu17 -I include libs/system/tests/test_license_area.c libs/system/cellSysutil.c
 * -Wl,-dead_strip -pthread -framework CoreFoundation -o /tmp/test_license_area && /tmp/test_license_area */
#include "../cellSysutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}
void cellMsgDialog_tick(void) {}
/* cellSysutil.c declares vm_write32 extern (the runtime defines it); the test stores the word big-endian like the runtime. */
void vm_write32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; i++) vm_base[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
void vm_write8(uint64_t a, uint8_t v) { vm_base[a] = v; }

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

/* The guest value is stored big-endian at value_ea (vm_write32). */
static uint32_t licence_area(void)
{
    s32 rc = cellSysutilGetSystemParamInt(CELL_SYSUTIL_SYSTEMPARAM_ID_LICENSE_AREA, 0x100);
    CHECK(rc == CELL_OK);
    return ((uint32_t)vm_base[0x100] << 24) | (vm_base[0x101] << 16) | (vm_base[0x102] << 8) | vm_base[0x103];
}

int main(void)
{
    vm_base = calloc(1, 65536);
    if (!vm_base) return 2;
    unsetenv("PS3_LICENSE_AREA");
    CHECK(licence_area() == CELL_SYSUTIL_LICENSE_AREA_A);   /* default: not Japan (0) */
    setenv("PS3_LICENSE_AREA", "2", 1);
    CHECK(licence_area() == CELL_SYSUTIL_LICENSE_AREA_E);
    setenv("PS3_LICENSE_AREA", "0", 1);
    CHECK(licence_area() == CELL_SYSUTIL_LICENSE_AREA_J);
    setenv("PS3_LICENSE_AREA", "", 1);                      /* empty = unset */
    CHECK(licence_area() == CELL_SYSUTIL_LICENSE_AREA_A);
    if (failures) return 1;
    puts("PASS: licence area default and PS3_LICENSE_AREA override");
    return 0;
}
