/* Missing slots must not masquerade as successful reads of stale previews. */
#include "../cellSaveData.c"
#include <assert.h>
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
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }
ps3_guest_caller_fn g_ps3_guest_caller;
static int loading, step, stat_calls, file_calls, force_error;
static const char payload[] = "independent save preview";
static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    assert(vm_read32(cb + 16) == 7);
    if (opd == 0x800) {
        ++stat_calls;
        if (force_error) { vm_write32(cb, CELL_SAVEDATA_CBRESULT_ERR_BROKEN); return; }
        if (loading && vm_read32(get + 4)) {
            vm_write32(cb, CELL_SAVEDATA_CBRESULT_ERR_NODATA); return;
        }
        if (!loading) {
            strcpy((char*)vm_base + get + 64, "Save test");
            vm_write32(set, (u32)get + 64);
        }
    } else {
        assert(opd == 0x900); ++file_calls;
        if (step++) { assert(vm_read32(get) == sizeof(payload)); vm_write32(cb, 1); return; }
        vm_write32(set, loading ? 0 : 1);
        vm_write32(set + 8, CELL_SAVEDATA_FILETYPE_NORMALFILE);
        vm_write32(set + 28, 0x300);
        vm_write32(set + 36, sizeof(payload));
        vm_write32(set + 40, sizeof(payload));
        vm_write32(set + 44, loading ? 0x500 : 0x400);
    }
}
static void compat(const char* value)
{
#ifdef _WIN32
    assert(_putenv_s("PS3_SAVEDATA_AUTOLOAD_NODATA_OK", value) == 0);
#else
    assert(setenv("PS3_SAVEDATA_AUTOLOAD_NODATA_OK", value, 1) == 0);
#endif
}
static s32 load(const char* name)
{
    loading = 1; step = 0;
    return cellSaveDataAutoLoad2(0, name, 0, (void*)0x200,
                                (void*)0x800, (void*)0x900, 0, (void*)7);
}
/* flOw's public bridge imports this legacy entry, not AutoLoad2. */
static s32 legacy_load(const char* name)
{
    loading = 1; step = 0;
    return cellSaveDataAutoLoad(0, name, 0, (void*)0x200,
                               (void*)0x800, (void*)0x900, 0, (void*)7);
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    assert(strlen(argv[1]) < sizeof(s_save_root)); strcpy(s_save_root, argv[1]);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    vm_write32(0x204, 4);
    strcpy((char*)vm_base + 0x300, "DATA.BIN");
    memcpy(vm_base + 0x400, payload, sizeof(payload));
    g_ps3_guest_caller = callback;
    /* Exact opt-in applies only to AutoLoad2. The legacy first-boot
     * callback and success result remain unchanged for every setting. */
    const char* settings[] = { "", "0", "1", "true", "01" };
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        compat(settings[i]);
        int before_stat = stat_calls, before_file = file_calls;
        assert(load("MISSING") == (!strcmp(settings[i], "1") ?
               CELL_OK : CELL_SAVEDATA_ERROR_NODATA));
        assert(legacy_load("MISSING") == CELL_OK);
        assert(stat_calls == before_stat + 2 && file_calls == before_file);
        force_error = 1;
        assert(load("MISSING") == CELL_SAVEDATA_ERROR_CBRESULT);
        assert(legacy_load("MISSING") == CELL_SAVEDATA_ERROR_CBRESULT);
        force_error = 0;
    }
    compat(""); loading = 0; step = 0;
    assert(cellSaveDataAutoSave2(0, "PRESENT", 0, (void*)0x200,
                (void*)0x800, (void*)0x900, 0, (void*)7) == CELL_OK);
    assert(load("PRESENT") == CELL_OK);
    assert(!memcmp(vm_base + 0x500, payload, sizeof(payload)));
    memset(vm_base + 0x500, 0, sizeof(payload));
    assert(legacy_load("PRESENT") == CELL_OK);
    assert(!memcmp(vm_base + 0x500, payload, sizeof(payload)));
    int before = file_calls;
    assert(load("MISSING") == CELL_SAVEDATA_ERROR_NODATA);
    assert(file_calls == before && !memcmp(vm_base + 0x500, payload, sizeof(payload)));
    assert(legacy_load("MISSING") == CELL_OK);
    assert(file_calls == before && !memcmp(vm_base + 0x500, payload, sizeof(payload)));
    /* Calling the legacy entry must not re-enable the override for AutoLoad2. */
    assert(load("MISSING") == CELL_SAVEDATA_ERROR_NODATA);
    free(vm_base);
    puts("PASS AutoLoad2 missing slots, unchanged legacy AutoLoad, exact opt-in, callbacks and real save/load round trip");
    return 0;
}
