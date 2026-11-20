/* cellFsWrite must not report an old failure on a later successful write
 * (libs/filesystem/cellFs.c). ferror() is sticky, so after a file-size limit
 * makes one write fail, the next write on the same fd did too even though it
 * wrote every byte.
 *
 * A soft RLIMIT_FSIZE makes the first write fail; the test then raises it and
 * writes again. The second call must return CELL_OK with a full nwrite. */
#include "../libs/filesystem/cellFs.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
void (*volatile g_ps3_guest_access_hook)(const void* host, size_t len);

/* cellFs.c's path fallback and its optional FSLOG backtrace hook. */
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
unsigned int ppu_prof_resolve_host(void* ra) { (void)ra; return 0; }

enum : uint32_t { PATH = 0x100, FDP = 0x500, NP = 0x508, WBUF = 0x1000 };

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static s32 fs_write(uint32_t fd, uint32_t buf, uint64_t n)
{
    return cellFsWrite((CellFsFd)fd, (const void*)(uintptr_t)buf, n, (u64*)(uintptr_t)NP);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x10000];
    vm_base = mem;
    ppu_vm_size = sizeof mem;

    if (argc < 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    char host[1200];
    snprintf(host, sizeof host, "%s/ps3recomp_cellfs_write_retry.bin", argv[1]);
    remove(host);

    cellfs_add_path_mapping("/dev_hdd1/", argv[1]);
    signal(SIGXFSZ, SIG_IGN);

    strcpy((char*)mem + PATH, "/dev_hdd1/ps3recomp_cellfs_write_retry.bin");
    s32 rc = cellFsOpen((const char*)(uintptr_t)PATH, 0x42,
                        (CellFsFd*)(uintptr_t)FDP, NULL, 0);
    const uint32_t fd = rd32(FDP);
    CHECK(rc == CELL_OK, "cellFsOpen(O_RDWR|O_CREAT) returns CELL_OK (fd %u)", fd);
    if (rc != CELL_OK) return 1;

    struct rlimit rl;
    rl.rlim_cur = 8192;
    rl.rlim_max = 1u << 20;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "set a soft RLIMIT_FSIZE");

    memset(mem + WBUF, 0xAB, 16384);
    rc = fs_write(fd, WBUF, 16384);
    CHECK(rc == CELL_EIO, "write past the limit returns CELL_EIO (0x%X)", (unsigned)rc);

    rl.rlim_cur = 1u << 20;
    rl.rlim_max = 1u << 20;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "raise RLIMIT_FSIZE");
    rc = fs_write(fd, WBUF, 4096);
    CHECK(rc == CELL_OK && rd64(NP) == 4096,
          "later write succeeds (rc 0x%X, nwrite %llu)", (unsigned)rc,
          (unsigned long long)rd64(NP));

    /* Grow past the limit first, so the small write below starts at or past it. */
    fs_write(fd, WBUF, 8192);
    /* A small write sits in the stdio buffer until a flush. */
    rl.rlim_cur = 8192;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "lower RLIMIT_FSIZE to the file's size");
    rc = fs_write(fd, WBUF, 16);
    CHECK(rc == CELL_EIO, "a small write past the limit fails too (0x%X)", (unsigned)rc);
    rl.rlim_cur = 1u << 20;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "raise RLIMIT_FSIZE again");
    rc = fs_write(fd, WBUF, 16);
    CHECK(rc == CELL_OK, "and the retry succeeds (0x%X)", (unsigned)rc);

    cellFsClose((CellFsFd)fd);
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
