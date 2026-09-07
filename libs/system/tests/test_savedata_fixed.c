/* clang -std=gnu17 -I include libs/system/tests/test_savedata_fixed.c
 * -Wl,-dead_strip -o /tmp/test_savedata_fixed && /tmp/test_savedata_fixed */
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


static unsigned fixed_calls, stat_calls;
static int mode;
static void guest_callback(u32 opd, u64 cb, u64 get, u64 set,
                           u64 a, u64 b, u64 c, u64 d, u64 e)
{
    assert(!a && !b && !c && !d && !e);
    assert(cb >= 0x100000 && cb < 0x120000);
    assert(vm_read32(cb) == 0); /* SDK OK_NEXT is zero */
    assert(vm_read32(cb + 16) == 0xCAFE);
    if (opd == 0x800) {
        fixed_calls++;
        assert(vm_read32(get) == 2 && vm_read32(get + 4) == 1);
        u32 list = vm_read32(get + 8);
        assert(list >= 0x100000 && list < 0x120000);
        assert(strncmp((char*)vm_base + list, "TEST", 4) == 0);
        assert(vm_base[get + 3] == 2); /* big-endian field, not host struct */
        for (u32 i = 12; i < 0x4C; i++) assert(vm_base[get + i] == 0);
        if (mode == 1) { vm_write32(cb, 1); return; } /* OK_LAST */
        if (mode == 2) { vm_write32(cb, (u32)-4); return; }
        if (mode == 3) return; /* No selected directory is invalid. */
        vm_write32(set, list); /* Name lives in scratch that funcStat reuses. */
        vm_write32(set + 8, 1);
    } else {
        assert(opd == 0x900); stat_calls++;
        assert(vm_read32(get + 4) == 0); /* existing directory */
        assert(strncmp((char*)vm_base + get + 32, "TEST", 4) == 0);
        for (u32 i = 1640; i < 1704; i++) assert(vm_base[get + i] == 0);
        vm_write32(cb, 1); /* Finish without a file operation. */
    }
}
int main(void)
{
    char root[] = "/tmp/ps3recomp-savedata-XXXXXX";
    assert(mkdtemp(root)); strcpy(s_save_root, root);
    char path[256];
    snprintf(path, sizeof(path), "%s/TEST00001", root); assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/TEST00002", root); assert(mkdir(path, 0700) == 0);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    assert(cellSaveData_set_scratch_region(0x100001, 0x20000) == CELL_SAVEDATA_ERROR_PARAM);
    assert(cellSaveData_set_scratch_region(0xFFFF0000, 0x20000) == CELL_SAVEDATA_ERROR_PARAM);
    strcpy((char*)vm_base + 0x300, "TEST");
    vm_write32(0x108, 0x300); vm_write32(0x200, 1);
    g_ps3_guest_caller = guest_callback;
    assert(cellSaveDataFixedLoad2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, NULL, 0, (void*)0xCAFE) == CELL_OK);
    assert(fixed_calls == 1 && stat_calls == 1);
    mode = 1;
    assert(cellSaveDataFixedSave2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, NULL, 0, (void*)0xCAFE) == CELL_OK);
    assert(fixed_calls == 2 && stat_calls == 1);
    mode = 2;
    assert(cellSaveDataFixedLoad2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, NULL, 0, (void*)0xCAFE) == CELL_SAVEDATA_ERROR_CBRESULT);
    assert(fixed_calls == 3 && stat_calls == 1);
    mode = 3;
    assert(cellSaveDataFixedLoad2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, NULL, 0, (void*)0xCAFE) == CELL_SAVEDATA_ERROR_PARAM);
    g_ps3_guest_caller = NULL;
    assert(cellSaveDataFixedLoad2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, NULL, 0, (void*)0xCAFE) == CELL_SAVEDATA_ERROR_INTERNAL);
    assert(fixed_calls == 4 && stat_calls == 1);
    snprintf(path, sizeof(path), "%s/TEST00001", root); assert(rmdir(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00002", root); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0); free(vm_base);
    puts("Fixed save/load guest callback ABI and completion checks passed");
}
