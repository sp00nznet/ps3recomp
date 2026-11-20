/* cellGemGetState with no Move controller connected answers CELL_GEM_NOT_CONNECTED (1), a tracking result the title
 * compares and keeps as the controller's status, not the error code CELL_GEM_ERROR_NOT_CONNECTED (0x80121804). A
 * title that gets anything but 1 for a missing controller treats it as one that needs calibrating.
 *
 * clang -std=gnu17 -I include libs/hardware/tests/test_gem_state.c
 * -Wl,-dead_strip -o /tmp/test_gem_state && /tmp/test_gem_state */
#include "../cellGem.c"
#include <assert.h>
#include <stdlib.h>
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

int main(void)
{
    vm_base = calloc(1, 0x10000);
    assert(vm_base);
    /* Guest addresses: the attribute is read through vm_read32, the state is only checked for NULL. */
    const CellGemAttribute* attr = (const CellGemAttribute*)(uintptr_t)0x100;
    CellGemState* state = (CellGemState*)(uintptr_t)0x200;
    vm_write32(0x100 + 4, 2);   /* maxConnect */

    assert((u32)cellGemGetState(0, 0, 0, state) == CELL_GEM_ERROR_NOT_INITIALIZED);
    assert(cellGemInit(attr) == CELL_OK);
    assert((u32)cellGemGetState(0, 0, 0, NULL) == CELL_GEM_ERROR_INVALID_ARGUMENT);

    s32 r = cellGemGetState(0, 0, 0, state);
    assert(r == CELL_GEM_NOT_CONNECTED);
    assert(r > 0);   /* a status, not an error (those are 0x8012xxxx, negative as s32) */

    assert(cellGemEnd() == CELL_OK);
    assert((u32)cellGemGetState(0, 0, 0, state) == CELL_GEM_ERROR_NOT_INITIALIZED);
    free(vm_base);
    puts("cellGemGetState answers CELL_GEM_NOT_CONNECTED for a missing controller");
    return 0;
}
