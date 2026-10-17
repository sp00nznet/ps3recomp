/* clang -std=gnu17 -I include libs/system/tests/test_savedata_root.c
 * -Wl,-dead_strip -o /tmp/test_savedata_root && /tmp/test_savedata_root */
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

int main(void)
{
    char root[] = "/tmp/ps3recomp-gamedata-root-XXXXXX";
    assert(mkdtemp(root));
    setenv("PS3_GAMEDATA_ROOT", root, 1);

    /* The env root replaces ./gamedata for the save path... */
    char path[1024], want[1024];
    build_save_path(path, sizeof(path), "TEST00000");
    snprintf(want, sizeof(want), "%s/dev_hdd0/home/00000001/savedata/TEST00000", root);
    assert(strcmp(path, want) == 0);

    /* ...and for the listing, which creates the tree and finds saves in it. */
    snprintf(want, sizeof(want), "%s/dev_hdd0/home/00000001/savedata", root);
    assert(enumerate_save_dirs("TEST", NULL, 0) == 0);
    assert(dir_exists(want));
    snprintf(path, sizeof(path), "%s/TEST00000", want);
    assert(HOST_MKDIR(path) == 0);
    CellSaveDataDirList list[2];
    assert(enumerate_save_dirs("TEST", list, 2) == 1);
    assert(strcmp(list[0].dirName, "TEST00000") == 0);

    /* Adopted once: a later change of the env does not move the saves. */
    setenv("PS3_GAMEDATA_ROOT", "/nonexistent", 1);
    assert(enumerate_save_dirs("TEST", list, 2) == 1);

    char cmd[1100];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    puts("PS3_GAMEDATA_ROOT moves the save root and the listing follows it");
}
