/* Compile with -ffunction-sections and linker garbage collection. */
#include "../cellSaveData.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; assert(!"Unexpected HLE registration in root fixture"); }

int main(int argc, char** argv)
{
    assert(argc == 2);
    assert(ps3_savedata_set_root(argv[1]) == CELL_OK);
    char before[1024]; strcpy(before, s_save_root);
    assert(ps3_savedata_set_root(NULL) == CELL_SAVEDATA_ERROR_PARAM);
    assert(ps3_savedata_set_root("relative/savedata") == CELL_SAVEDATA_ERROR_PARAM);
    char oversized[1025]; memset(oversized, 'x', sizeof oversized); oversized[1024] = 0;
    assert(ps3_savedata_set_root(oversized) == CELL_SAVEDATA_ERROR_PARAM);
    assert(strcmp(before, s_save_root) == 0);
    char trailing[1024]; assert(strlen(before) + 2 < sizeof trailing);
    snprintf(trailing, sizeof trailing, "%s/", before);
    assert(ps3_savedata_set_root(trailing) == CELL_OK);
    assert(strcmp(before, s_save_root) == 0);
    ensure_dirs(s_save_root);
    assert(dir_exists(s_save_root));
    char target[1024]; build_save_path(target, sizeof target, "TEST00000");
    assert(strstr(target, "TEST00000"));
    puts("Savedata root: absolute configuration, rejection preservation and contained parent creation pass");
}
