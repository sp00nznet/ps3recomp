/* clang -std=gnu17 -I include libs/system/tests/test_savedata_io.c
 * -Wl,-dead_strip -o /tmp/test_savedata_io && /tmp/test_savedata_io */
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

ps3_guest_caller_fn g_ps3_guest_caller;


static unsigned step;
static int loading;
static const char payload[] = "save round trip";
static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    assert(!a && !b && !c && !d && !e);
    if (opd == 0x800) {
        assert(vm_read32(cb + 16) == 7);
        vm_write32(cb + 16, 0xBEEF);
        vm_write32(set, 0x300); vm_write32(set + 8, 1);
    } else if (opd == 0x900) {
        assert(vm_read32(cb + 16) == 0xBEEF);
        assert(vm_read32(get + 4) == (loading ? 0 : 1));
        if (!loading) {
            strcpy((char*)vm_base + get + 64, "Save test");
            vm_write32(get + 64 + 1280, 0x1234);
            vm_write32(set, get + 64);
        } else {
            assert(strcmp((char*)vm_base + get + 64, "Save test") == 0);
            assert(vm_read32(get + 64 + 1280) == 0x1234);
            u32 n = vm_read32(get + 1632), list = vm_read32(get + 1636);
            assert(n == 2 && vm_read32(get + 1628) == 2);
            int found = 0, icon = 0;
            for (u32 i = 0; i < n; i++) {
                if (!strcmp((char*)vm_base + list + i * 56 + 40, "ICON0.PNG")) {
                    assert(vm_read32(list + i * 56) == CELL_SAVEDATA_FILETYPE_CONTENT_ICON0); icon = 1;
                }
                if (!strcmp((char*)vm_base + list + i * 56 + 40, "DATA.BIN")) {
                    assert(vm_read64(list + i * 56 + 8) == sizeof(payload)); found = 1;
                }
            }
            assert(found && icon);
        }
        vm_write32(cb + 16, 0xCAFE);
    } else {
        assert(opd == 0xA00);
        assert(vm_read32(cb + 16) == (step ? 0xCAFE + step : 0xCAFE));
        assert(vm_read32(get) == (step ? sizeof(payload) : 0));
        if (step == 2) { vm_write32(cb, 1); step++; return; }
        vm_write32(set, loading ? 0 : 1);
        vm_write32(set + 8, step ? 2 : 1); /* ICON0 has no explicit filename. */
        if (!step) vm_write32(set + 28, 0x340);
        vm_write32(set + 36, sizeof(payload));
        vm_write32(set + 40, sizeof(payload));
        vm_write32(set + 44, loading ? 0x500 + step * 0x100 : 0x400);
        step++; vm_write32(cb + 16, 0xCAFE + step);
    }
}
int main(void)
{
    char root[] = "/tmp/ps3recomp-savedata-io-XXXXXX";
    assert(mkdtemp(root)); strcpy(s_save_root, root);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    strcpy((char*)vm_base + 0x300, "TEST00000");
    strcpy((char*)vm_base + 0x340, "DATA.BIN");
    memcpy(vm_base + 0x400, payload, sizeof(payload));
    vm_write32(0x108, 0x300); vm_write32(0x200, 4); vm_write32(0x204, 2);
    g_ps3_guest_caller = callback;
    assert(cellSaveDataFixedSave2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, (void*)0xA00, 0, (void*)7) == CELL_OK);
    assert(step == 3);
    char path[256], bytes[sizeof(payload)];
    snprintf(path, sizeof(path), "%s/TEST00000/DATA.BIN", root);
    FILE* f = fopen(path, "rb"); assert(f);
    assert(fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes)); fclose(f);
    assert(memcmp(bytes, payload, sizeof(payload)) == 0);
    loading = 1; step = 0;
    assert(cellSaveDataFixedLoad2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, (void*)0xA00, 0, (void*)7) == CELL_OK);
    assert(step == 3);
    assert(memcmp(vm_base + 0x500, payload, sizeof(payload)) == 0);
    assert(memcmp(vm_base + 0x600, payload, sizeof(payload)) == 0);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00000/ICON0.PNG", root); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00000/PARAM.SFO", root); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00000", root); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0); free(vm_base);
    puts("Save/load round trip, callback userdata, metadata, and byte-count checks passed");
}
