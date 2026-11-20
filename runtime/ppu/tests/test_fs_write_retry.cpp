/* cellFsWrite on one fd must not report an old failure on a later successful
 * write (runtime/ppu/ppu_fs.cpp). ferror() is sticky: after a file-size limit
 * makes one write fail, the next write on the same fd returned EIO even
 * though it wrote every byte.
 *
 * A soft RLIMIT_FSIZE makes the first write fail; the test then raises it and
 * writes again. The second call must return CELL_OK with a full nwrite.
 *
 *   tools/test_ppu_fs_write_retry.py [--exe-out PATH]
 *   test_fs_write_retry DIR
 */
#include "ppu_fs.cpp"
#include <signal.h>
#include <sys/resource.h>

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void (*volatile g_ps3_guest_access_hook)(const void* host, size_t len) = nullptr;
void (*volatile g_ps3_guest_write_hook)(const void* host, size_t len) = nullptr;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}
/* ps3_dev_flash_path (runtime/platform), which host_path calls for /dev_flash. */
#include "../../platform/ps3_dev_flash.c"
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

enum : uint32_t { PATH = 0x100, FDP = 0x500, NP = 0x508, WBUF = 0x1000 };

static uint32_t fs_write(uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsWrite(&ctx);
    return (uint32_t)ctx.gpr[3];
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x10000];
    vm_base = mem;
    ppu_vm_size = sizeof mem;

    if (argc < 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    char host[1200];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_write_retry.bin", argv[1]);
    remove(host);

    ppu_vfs_root = argv[1];
    signal(SIGXFSZ, SIG_IGN);

    strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_fs_write_retry.bin");
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = PATH; ctx.gpr[4] = 0x42; ctx.gpr[5] = FDP;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(FDP);
    CHECK((uint32_t)ctx.gpr[3] == CELL_OK,
          "cellFsOpen(O_RDWR|O_CREAT) returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != CELL_OK) return 1;

    struct rlimit rl;
    rl.rlim_cur = 8192;
    rl.rlim_max = 1u << 20;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "set a soft RLIMIT_FSIZE");

    memset(mem + WBUF, 0xAB, 16384);
    uint32_t rc = fs_write(fd, WBUF, 16384);
    CHECK(rc == (uint32_t)CELL_FS_EIO, "write past the limit returns CELL_FS_EIO (0x%X)", rc);

    rl.rlim_cur = 1u << 20;
    rl.rlim_max = 1u << 20;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "raise RLIMIT_FSIZE");
    rc = fs_write(fd, WBUF, 4096);
    CHECK(rc == CELL_OK && rd64(NP) == 4096,
          "later write succeeds (rc 0x%X, nwrite %llu)", rc,
          (unsigned long long)rd64(NP));

    /* Grow past the limit first, so the small write below starts at or past it. */
    fs_write(fd, WBUF, 8192);
    /* A small write sits in the stdio buffer until a flush. */
    rl.rlim_cur = 8192;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "lower RLIMIT_FSIZE to the file's size");
    rc = fs_write(fd, WBUF, 16);
    CHECK(rc == (uint32_t)CELL_FS_EIO, "a small write past the limit fails too (0x%X)", (unsigned)rc);
    rl.rlim_cur = 1u << 20;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "raise RLIMIT_FSIZE again");
    rc = fs_write(fd, WBUF, 16);
    CHECK(rc == CELL_OK, "and the retry succeeds (0x%X)", (unsigned)rc);

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
