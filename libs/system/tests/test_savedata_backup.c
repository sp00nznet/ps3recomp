/* clang -std=gnu17 -I include libs/system/tests/test_savedata_backup.c
 * -Wl,-dead_strip -o /tmp/test_savedata_backup && /tmp/test_savedata_backup
 * (on Linux: -ffunction-sections -Wl,--gc-sections instead of -dead_strip) */
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

static void put(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void expect(const char* path, const char* text)
{
    char buf[64] = {0};
    FILE* f = fopen(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    assert(n == strlen(text) && !memcmp(buf, text, n));
}

int main(void)
{
    char root[] = "/tmp/ps3recomp-backup-XXXXXX";
    assert(mkdtemp(root));
    unsetenv("PS3_GAMEDATA_ROOT");
    setenv("PS3_HDD0_ROOT", root, 1);
    setenv("PS3_SAVEDATA_BACKUPS", "2", 1);

    char save[1024], file[1100];
    build_save_path(save, sizeof save, "TEST00000");
    ensure_dirs(save);
    /* PARAM.SFO is left out of the listing a title sees; the copies need it,
     * and a pruned copy that still held it could not be removed. */
    snprintf(file, sizeof file, "%s/PARAM.SFO", save);
    put(file, "sfo");
    snprintf(file, sizeof file, "%s/DATA00.BIN", save);

    /* Three saves over one directory keep the two newest copies, each with
     * the contents the directory held before that save. */
    const char* const before[3] = { "first", "second", "third" };
    for (int i = 0; i < 3; i++) {
        put(file, before[i]);
        save_backup(save, "TEST00000");
    }
    char broot[1100];
    snprintf(broot, sizeof broot, "%s/home/00000001/savedata_backup/TEST00000", root);
    char names[8][64];
    assert(list_subdirs(broot, names, 8) == 2);
    snprintf(file, sizeof file, "%s/%s/DATA00.BIN", broot, names[0]);
    expect(file, "second");
    snprintf(file, sizeof file, "%s/%s/DATA00.BIN", broot, names[1]);
    expect(file, "third");
    snprintf(file, sizeof file, "%s/%s/PARAM.SFO", broot, names[1]);
    expect(file, "sfo");

    char cmd[1100];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    puts("PS3_SAVEDATA_BACKUPS keeps the newest copies of a save before it is written over");
}
