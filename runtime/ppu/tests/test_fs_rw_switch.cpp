/*
 * ps3recomp - cellFs read after write, and write after read, on one fd
 * (ppu_fs.cpp).
 *
 * ppu_fs.cpp keeps each file as a stdio stream. C leaves a read right after a
 * write (with no flush or seek between them) undefined, and the same for a
 * write right after a read; cellFs has no such rule. FIOS creates its HDD
 * cache index, writes a 512-byte header and reads on: on the MSVC CRT the read
 * returned 3584 bytes of stale buffer and the header never reached the file.
 * This fixture drives cellFsOpen / cellFsWrite / cellFsRead / cellFsFstat /
 * cellFsLseek through that sequence and checks what the guest and the host
 * file see. The undefined case shows on the MSVC CRT; other C libraries may
 * pass either way.
 *
 *   tools/test_ppu_fs_rw_switch.py [--exe-out PATH]
 *   test_fs_rw_switch [DIR]      (DIR: where the test file goes; default the
 *                                 system temp directory)
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
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

/* Guest memory layout used below. */
enum : uint32_t { PATH = 0x100, FDP = 0x500, NP = 0x508, POSP = 0x510, STATB = 0x600,
                  WBUF = 0x1000, RBUF = 0x2000 /* 64 KB read buffer */ };

static uint64_t fs_write(uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsWrite(&ctx);
    return rd64(NP);
}
static uint64_t fs_read(uint32_t fd, uint32_t buf, uint64_t n)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = NP;
    cellFsRead(&ctx);
    return rd64(NP);
}
static uint64_t fs_seek(uint32_t fd, int64_t off)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = (uint64_t)off; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = POSP;
    cellFsLseek(&ctx);
    return rd64(POSP);
}
static uint64_t fs_size(uint32_t fd)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = STATB;
    cellFsFstat(&ctx);
    return rd64(STATB + 0x24);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x20000];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : NULL;
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (!dir) { DWORD n = GetTempPathA(sizeof tmp, tmp); if (n && n < sizeof tmp) { tmp[n - 1] = 0; dir = tmp; } }
#endif
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_rw_switch.idx", dir);
    remove(host);

    ppu_vfs_root = dir;
    strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_fs_rw_switch.idx");

    /* FIOS's open: O_RDWR | O_CREAT on a file that does not exist yet. */
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = PATH; ctx.gpr[4] = 0x42; ctx.gpr[5] = FDP;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(FDP);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen(O_RDWR|O_CREAT) returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != 0) { printf("FAIL (could not create %s)\n", host); return 1; }

    /* Write a 512-byte header, then read straight on: the file ends at the
     * header, so the read finds nothing and the header stays written. */
    /* CHECK evaluates its condition twice: every call that moves the file
     * runs once, before it. */
    memset(mem + WBUF, 0xAB, 512);
    const uint64_t wrote = fs_write(fd, WBUF, 512);
    CHECK(wrote == 512, "cellFsWrite writes the 512-byte header (%llu)", (unsigned long long)wrote);
    memset(mem + RBUF, 0xEE, 0x10000);
    const uint64_t got = fs_read(fd, RBUF, 0x10000);
    CHECK(got == 0, "cellFsRead right after the write reads %llu bytes at end of file (want 0)",
          (unsigned long long)got);
    const uint64_t size = fs_size(fd);
    CHECK(size == 512, "cellFsFstat after the read: size %llu (want 512)", (unsigned long long)size);

    /* Read the header back, then write right after the read: the write lands
     * where the read stopped. */
    const uint64_t pos = fs_seek(fd, 0);
    CHECK(pos == 0, "cellFsLseek back to 0");
    const uint64_t back = fs_read(fd, RBUF, 16);
    CHECK(back == 16 && mem[RBUF] == 0xAB && mem[RBUF + 15] == 0xAB, "cellFsRead returns the header bytes");
    memset(mem + WBUF, 0xCD, 16);
    const uint64_t wrote2 = fs_write(fd, WBUF, 16);
    CHECK(wrote2 == 16, "cellFsWrite right after the read writes 16 bytes");

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);

    /* What reached the host file. */
    uint8_t disk[600]; size_t dn = 0;
    if (FILE* f = fopen(host, "rb")) { dn = fread(disk, 1, sizeof disk, f); fclose(f); }
    CHECK(dn == 512, "host file is %zu bytes (want 512)", dn);
    CHECK(dn >= 32 && disk[0] == 0xAB && disk[15] == 0xAB && disk[16] == 0xCD && disk[31] == 0xCD &&
          (dn < 33 || disk[32] == 0xAB),
          "host file: header, then the 16 bytes written after the read at offset 16");
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
