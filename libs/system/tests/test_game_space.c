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
const char* ppu_vfs_root = ".";
int main(void)
{
    vm_base = calloc(1, 65536); assert(vm_base);
    cellGame_set_content_path("/ps3recomp-missing-space-test/content/game");
    memcpy(vm_base + 0x100, "TEST00000", 10);
    assert(cellGameDataCheck(3, (void*)0x100, (void*)0x200) == CELL_GAME_RET_NONE);
    assert(queries == 4);
    /* 12 GiB free: hddFreeSizeKB * 1024 would wrap in 32 bits, so the guest
     * gets RPCS3's 40 GiB - 256 KiB, whose x1024 is 0xFFFC0000. */
    assert(content_free_kb() == 12 * 1024 * 1024);
    assert(vm_read32(0x200) == 40u * 1024u * 1024u - 256u);
    assert((uint32_t)(vm_read32(0x200) * 1024u) == 0xFFFC0000u);
    assert(vm_read32(0x204) == 0 && vm_read32(0x208) == 0);
    assert(vm_base[0x200] == 0x02 && vm_base[0x201] == 0x7F);   /* big-endian 0x027FFF00 */
    /* Below 4 GiB the real figure cannot wrap and is reported as is. */
    blocks = 1024 * 1024 - 1;   /* 4 GiB - 4 KiB */
    assert(content_free_kb_reported() == 4 * 1024 * 1024 - 4);
    blocks = 1024 * 1024;       /* exactly 4 GiB: x1024 = 2^32 wraps to 0 */
    assert(content_free_kb_reported() == 40 * 1024 * 1024 - 256);
    blocks = 3ULL * 1024 * 1024;
    setenv("PS3_GAME_FREE_SPACE_RAW", "1", 1);   /* the measured value, as before */
    assert(content_free_kb_reported() == 12 * 1024 * 1024);
    unsetenv("PS3_GAME_FREE_SPACE_RAW");
    blocks = UINT64_MAX;
    assert(content_free_kb() == INT32_MAX);
    blocks = 0; assert(content_free_kb() == 0);
    blocks = 256; block_size = 0; assert(content_free_kb() == 1024);
    denied = 1; queries = 0;
    assert(content_free_kb() == 0 && queries == 1);
    free(vm_base);
    puts("Game storage availability and guest-endian checks passed");
}
