/* cellGcmMapMainMemory's IO offsets come from the IO page table (first fit), and no map happens before cellGcmInit.
 * Includes the real implementation and drives its public entry points; the expected numbers are the ones the
 * firmware/reference emulator hand out, not derived from the code under test.
 *
 * Scenario (a title that maps before it initialises): MapMainMemory(0x40000000, 2MB) before cellGcmInit, then
 * cellGcmInit with an 8MB IO window at io 0 (ea 0x40300000), then more maps. With a bump pointer kept across Init the
 * pre-Init map moved it to 0x300000, and the next map landed on io 0x300000 inside the Init window. */
#include "../libs/video/cellGcmSys.c"
#include "../libs/video/rsx_tiling.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int g_resv_store_active;
void ppu_resv_break_store(uint64_t addr) { (void)addr; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);   /* set by cellGcmInit */

#define MB 0x100000u
#define OUT ((u32*)(uintptr_t)0x100)

/* Failures are reported and the run goes on (the first 12 are printed; the exit status says so at the end), so one run lists everything wrong. */
static int g_fails;
#define EXPECT(cond) do { if (!(cond)) { if (g_fails++ < 12) fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define EXPECT_EQ(got, want) do { unsigned long long g_ = (got), w_ = (want); if (g_ != w_) { \
    if (g_fails++ < 12) fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, g_, w_); } } while (0)

/* The state before any cellGcmInit. The tables are 0xFFFF = unmapped here (the statics start zeroed, which reads as
 * "mapped to page 0" before Init wipes them; that is a separate matter and not what this test is about). */
static void power_on(void)
{
    memset(&s_config, 0, sizeof(s_config));
    memset(s_io_mappings, 0, sizeof(s_io_mappings)); s_io_mapping_count = 0;
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_gcm_initialized = 0;
}
static u32 map(u32 ea, u32 size)
{
    vm_write32(0x100, 0xDEADBEEFu);
    s32 rc = cellGcmMapMainMemory(ea, size, OUT);
    EXPECT_EQ((u32)rc, CELL_OK);
    return vm_read32(0x100);
}

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    ppu_hle_inject_base = 0x1000;

    /* --- the title's exact sequence, whatever the pre-Init map answers: no later map may overlap the Init window --- */
    power_on();
    (void)cellGcmMapMainMemory(0x40000000, 2 * MB, OUT);
    EXPECT_EQ((u32)cellGcmInit(0x100000, 8 * MB, 0x40300000), (u32)CELL_OK);
    {
        u32 io = map(0x40D00000, 6 * MB);
        EXPECT(io >= 8 * MB);                              /* the window is io 0..8MB */
        EXPECT_EQ(gcm_io2ea(0x300000), 0x40600000u);       /* still the window's own page */
    }

    /* --- a map before cellGcmInit is refused, and changes nothing --- */
    power_on();
    vm_write32(0x100, 0xDEADBEEFu);
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40000000, 2 * MB, OUT), (u32)CELL_GCM_ERROR_NO_IO_PAGE_TABLE);
    EXPECT_EQ(vm_read32(0x100), 0xDEADBEEFu);          /* *offset untouched */
    EXPECT_EQ(s_io_mapping_count, 0);
    EXPECT_EQ(s_io_address_table[0x400], 0xFFFFu);     /* nothing populated either way */
    EXPECT_EQ(s_ea_address_table[1], 0xFFFFu);
    /* AddressToOffset's auto-map of low main memory does not run before Init either */
    EXPECT((u32)cellGcmAddressToOffset(0x10000000, OUT) != (u32)CELL_OK);
    EXPECT_EQ(s_io_mapping_count, 0);

    /* --- Init with an 8MB window at io 0 (ea 0x40300000), then the maps land after it, first fit --- */
    EXPECT_EQ((u32)cellGcmInit(0x100000, 8 * MB, 0x40300000), (u32)CELL_OK);
    EXPECT_EQ(map(0x40D00000, 6 * MB), 0x800000u);     /* right after the window, not io 0x300000 */
    EXPECT_EQ(map(0x40000000, 2 * MB), 0xE00000u);
    EXPECT_EQ(map(0x41000000, 1 * MB), 0x1000000u);
    /* the window is intact: io 0..8MB still resolves to ea 0x40300000.. and no map overlaps it */
    for (u32 io = 0; io < 8 * MB; io += MB) {
        EXPECT_EQ(gcm_io2ea(io), 0x40300000u + io);
        EXPECT_EQ(cellGcmAddressToOffset(0x40300000u + io + 0x123, OUT), (u32)CELL_OK);
        EXPECT_EQ(vm_read32(0x100), io + 0x123);
    }
    EXPECT_EQ(gcm_io2ea(0x800000), 0x40D00000u);
    EXPECT_EQ(gcm_io2ea(0xD00000), 0x40D00000u + 5 * MB);
    EXPECT_EQ(gcm_io2ea(0xE00000), 0x40000000u);
    EXPECT_EQ(gcm_io2ea(0x1000000), 0x41000000u);
    EXPECT_EQ(cellGcmAddressToOffset(0x40D00000u + 0x456, OUT), (u32)CELL_OK);
    EXPECT_EQ(vm_read32(0x100), 0x800456u);

    /* --- first fit reuses holes and only a run that is big enough --- */
    power_on();
    EXPECT_EQ((u32)cellGcmInit(0x100000, 8 * MB, 0x40300000), (u32)CELL_OK);
    EXPECT_EQ(map(0x50000000, MB), 0x800000u);
    EXPECT_EQ(map(0x50100000, MB), 0x900000u);
    EXPECT_EQ(map(0x50200000, MB), 0xA00000u);
    EXPECT_EQ((u32)cellGcmUnmapEaIoAddress(0x50100000), (u32)CELL_OK);   /* a 1MB hole at io 0x900000 */
    EXPECT_EQ(map(0x50300000, 2 * MB), 0xB00000u);     /* the hole is too small for 2MB */
    EXPECT_EQ(map(0x50500000, MB), 0x900000u);         /* a 1MB map takes the hole */

    /* --- no Init window to speak of: io 0 is still the FIFO's own page and is never handed out --- */
    power_on();
    EXPECT_EQ((u32)cellGcmInit(0x10000, 0, 0), (u32)CELL_OK);
    EXPECT_EQ(map(0x50000000, MB), 0x100000u);

    /* --- IO space full: refused (not a bump past the 256MB window), nothing leaked --- */
    power_on();
    EXPECT_EQ((u32)cellGcmInit(0x100000, 8 * MB, 0x40300000), (u32)CELL_OK);
    unsigned n = 0;
    for (; n < 256; n++) {
        vm_write32(0x100, 0xDEADBEEFu);
        s32 rc = cellGcmMapMainMemory(0x60000000u + n * MB, MB, OUT);
        if (rc != CELL_OK) {
            EXPECT_EQ((u32)rc, (u32)CELL_GCM_ERROR_NO_IO_PAGE_TABLE);
            EXPECT_EQ(vm_read32(0x100), 0xDEADBEEFu);
            break;
        }
        EXPECT_EQ(vm_read32(0x100), 0x800000u + n * MB);
    }
    EXPECT_EQ(n, 248u);                                /* io pages 8..255 */
    EXPECT_EQ(s_io_mapping_count, 1 + 248);
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x70000000u, 2 * MB, OUT), (u32)CELL_GCM_ERROR_NO_IO_PAGE_TABLE);
    EXPECT_EQ((u32)cellGcmUnmapEaIoAddress(0x60000000u + 5 * MB), (u32)CELL_OK);
    EXPECT_EQ(map(0x70000000u, MB), 0x800000u + 5 * MB);   /* the freed page is found again */

    free(vm_base);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: GCM IO first fit, map refused before Init and when IO space is full, Init window kept");
    return 0;
}
