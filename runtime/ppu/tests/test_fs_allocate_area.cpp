/*
 * ps3recomp - cellFsAllocateFileAreaWithoutZeroFill (ppu_fs.cpp).
 *
 * After the call the file is at least the requested size: a title that sizes a
 * cache file this way reads the size back later and rebuilds the cache when it
 * is short. Drives the handler through ppu_fs.cpp and checks:
 *   - a short file grows to the size, keeps its data, and reads the new space as zeroes;
 *   - a file already longer is left as it is;
 *   - a missing file is not created, and the call still returns CELL_OK.
 *
 *   tools/test_ppu_fs_allocate_area.py [--exe-out PATH]
 *   test_fs_allocate_area [DIR]   (DIR: where the files go; default the system temp directory)
 */
#include "ppu_fs.cpp"
#ifdef _WIN32
#  include <windows.h>
#endif

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}
/* ps3_dev_flash_path (runtime/platform), which host_path calls for /dev_flash: the real one, it needs nothing. */
#include "../../platform/ps3_dev_flash.c"
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static uint8_t mem[1 << 16];

static uint32_t allocate(const char* gpath, uint64_t size)
{
    strcpy((char*)mem + 0x100, gpath);
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0x100;
    ctx.gpr[4] = size;
    cellFsAllocateFileAreaWithoutZeroFill(&ctx);
    return (uint32_t)ctx.gpr[3];
}

static int64_t host_size(const char* path)
{
    fs_stat64_t st;
    return FS_STAT64(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static int write_file(const char* path, const char* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : NULL;
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (!dir) { DWORD n = GetTempPathA(sizeof tmp, tmp); if (n && n < sizeof tmp) { tmp[n - 1] = 0; dir = tmp; } }
#endif
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    ppu_vfs_root = dir;

    const uint64_t GROW = 64ull << 20;   /* 64 MiB */
    char shorter[1100], longer[1100], missing[1100];
    snprintf(shorter, sizeof shorter, "%s/ps3recomp_fs_area_short.bin", dir);
    snprintf(longer,  sizeof longer,  "%s/ps3recomp_fs_area_long.bin", dir);
    snprintf(missing, sizeof missing, "%s/ps3recomp_fs_area_missing.bin", dir);
    remove(missing);
    if (write_file(shorter, "HEAD", 4) || write_file(longer, "0123456789", 10)) {
        printf("FAIL: could not create the test files under %s\n", dir);
        return 1;
    }

    /* A short file grows to the size, keeps its data, and the new space reads as zeroes. */
    uint32_t rc = allocate("/dev_hdd1/ps3recomp_fs_area_short.bin", GROW);
    CHECK(rc == 0, "short file: returns CELL_OK (0x%08X)", rc);
    CHECK(host_size(shorter) == (int64_t)GROW, "short file: size %lld (want %llu)",
          (long long)host_size(shorter), (unsigned long long)GROW);
    {
        FILE* f = fopen(shorter, "rb");
        char head[4] = {0};
        unsigned char tail[16];
        int ok = f && fread(head, 1, 4, f) == 4 &&
                 FS_SEEK64(f, GROW - sizeof tail, SEEK_SET) == 0 &&
                 fread(tail, 1, sizeof tail, f) == sizeof tail;
        if (f) fclose(f);
        int zero = 1;
        for (size_t i = 0; ok && i < sizeof tail; i++) zero &= tail[i] == 0;
        CHECK(ok && memcmp(head, "HEAD", 4) == 0, "short file: its first bytes are kept");
        CHECK(ok && zero, "short file: the new space reads as zeroes");
    }

    /* A second call with the same size changes nothing. */
    rc = allocate("/dev_hdd1/ps3recomp_fs_area_short.bin", GROW);
    CHECK(rc == 0 && host_size(shorter) == (int64_t)GROW, "same size again: CELL_OK, size unchanged");

    /* A file already longer than the request is left as it is. */
    rc = allocate("/dev_hdd1/ps3recomp_fs_area_long.bin", 4);
    CHECK(rc == 0 && host_size(longer) == 10, "longer file: CELL_OK, size %lld (want 10)",
          (long long)host_size(longer));

    /* A missing file is not created. */
    rc = allocate("/dev_hdd1/ps3recomp_fs_area_missing.bin", GROW);
    CHECK(rc == 0 && host_size(missing) < 0, "missing file: CELL_OK, nothing created");

    remove(shorter);
    remove(longer);
    remove(missing);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
