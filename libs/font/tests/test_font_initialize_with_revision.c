/*
 * test_font_initialize_with_revision - cellFontInitializeWithRevision.
 *
 * Titles built with a newer SDK start the font library with
 * cellFontInitializeWithRevision(revisionFlags, config) instead of cellFontInit.
 * It must initialise the library: without it every later cellFont call returns
 * CELL_FONT_ERROR_UNINITIALIZED, which a title that ignores the result never
 * notices.
 *
 *   cc -std=gnu17 -I include -o test_font_initialize_with_revision libs/font/tests/test_font_initialize_with_revision.c -lm
 */
#include "../cellFont.c"

#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void spu_linelock_lock(uint32_t addr) { (void)addr; }
void spu_linelock_release(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

#define FONT_TYPE 0x1000u
#define FONT_OUT  0x2000u

int main(void)
{
    static uint8_t mem[0x100000];
    vm_base = mem;
    ppu_vm_size = sizeof mem;

    CellFontType* type = (CellFontType*)(uintptr_t)FONT_TYPE;     /* guest addresses, as the adapter passes them */
    CellFont* font = (CellFont*)(uintptr_t)FONT_OUT;

    /* before any init the library is down */
    assert(cellFontOpenFontset(0, type, font) == CELL_FONT_ERROR_UNINITIALIZED);

    assert(cellFontInitializeWithRevision(1, NULL) == CELL_OK);
    assert(cellFontOpenFontset(0, type, font) != CELL_FONT_ERROR_UNINITIALIZED);

    /* it is the same initialisation as cellFontInit: a second one is refused */
    assert(cellFontInit(NULL) == CELL_FONT_ERROR_ALREADY_INITIALIZED);
    assert(cellFontInitializeWithRevision(1, NULL) == CELL_FONT_ERROR_ALREADY_INITIALIZED);

    /* and cellFontEnd takes it down again */
    assert(cellFontEnd() == CELL_OK);
    assert(cellFontOpenFontset(0, type, font) == CELL_FONT_ERROR_UNINITIALIZED);

    printf("test_font_initialize_with_revision: all passed\n");
    return 0;
}
