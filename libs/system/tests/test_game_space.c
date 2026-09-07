/* macOS: clang -std=gnu17 -I include libs/system/tests/test_game_space.c
 * -Wl,-dead_strip -o /tmp/test_game_space && /tmp/test_game_space */
#include <sys/statvfs.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
static unsigned queries;
static unsigned long long blocks = 3ULL * 1024 * 1024;
static unsigned long block_size = 4096;
static int denied;
static int test_statvfs(const char* path, struct statvfs* out)
{
    queries++;
    if (denied) { errno = EACCES; return -1; }
    if (strcmp(path, "/") != 0) { errno = ENOENT; return -1; }
    memset(out, 0, sizeof(*out));
    out->f_frsize = block_size;
    out->f_bsize = 4096;
    out->f_bavail = blocks;
    return 0;
}
#define statvfs(path, out) test_statvfs(path, out)
#include "../cellGame.c"
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
    vm_base = calloc(1, 65536); assert(vm_base);
    cellGame_set_content_path("/ps3recomp-missing-space-test/content/game");
    memcpy(vm_base + 0x100, "TEST00000", 10);
    assert(cellGameDataCheck(3, (void*)0x100, (void*)0x200) == CELL_GAME_RET_NONE);
    assert(queries == 4);
    assert(vm_read32(0x200) == 12u * 1024u * 1024u);
    assert(vm_read32(0x204) == 0 && vm_read32(0x208) == 0);
    assert(vm_base[0x200] == 0 && vm_base[0x201] == 0xC0);
    blocks = UINT64_MAX;
    assert(content_free_kb() == INT32_MAX);
    blocks = 0; assert(content_free_kb() == 0);
    blocks = 256; block_size = 0; assert(content_free_kb() == 1024);
    denied = 1; queries = 0;
    assert(content_free_kb() == 0 && queries == 1);
    free(vm_base);
    puts("Game storage availability and guest-endian checks passed");
}
