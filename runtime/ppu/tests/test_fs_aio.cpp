/*
 * ps3recomp - cellFsAioRead on a shared fd (ppu_fs.cpp).
 *
 * An AIO read names its own offset. It must not move the fd's position for
 * the plain cellFsRead that follows, and several guest threads (FIOS media
 * threads, audio streamers) issue them on one fd at once. The fixture writes
 * a file whose every 4-byte word holds its own offset, then checks both:
 * a cellFsRead after an AIO read continues from the fd's position, and
 * reads racing from four threads each get the words they asked for.
 *
 *   tools/test_ppu_fs_aio.py
 *   test_fs_aio [DIR]     (DIR: where the file goes; default the temp dir)
 */
#include "ppu_fs.cpp"
#include <thread>
#include <vector>

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

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
static void wr32(uint32_t a, uint32_t v) { vm_write32(a, v); }
static void wr64(uint32_t a, uint64_t v) { vm_write64(a, v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static const uint32_t WORDS = 1u << 16;   /* 256 KiB file */

/* One AIO read of `len` bytes at `off` into guest `buf`, through a CellFsAio
 * at guest `aio`: {fd u32, pad, offset u64, buf u32, pad, size u64}. */
static void aio_read(uint32_t aio, uint32_t fd, uint64_t off, uint32_t buf, uint64_t len)
{
    wr32(aio + 0x00, fd); wr64(aio + 0x08, off); wr32(aio + 0x10, buf); wr64(aio + 0x18, len);
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = aio; ctx.gpr[4] = 0; ctx.gpr[5] = 0;
    cellFsAioRead(&ctx);
}

/* Every word at file offset o holds o (big-endian), so a buffer read from
 * offset `off` is right iff word i holds off + 4*i. */
static bool holds(uint32_t buf, uint64_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) if (rd32(buf + i) != (uint32_t)(off + i)) return false;
    return true;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[1 << 20];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_aio.bin", dir);
    {
        FILE* f = fopen(host, "wb");
        if (!f) { printf("FAIL: could not create %s\n", host); return 1; }
        for (uint32_t i = 0; i < WORDS; i++) { uint32_t be = __builtin_bswap32(i * 4); fwrite(&be, 4, 1, f); }
        fclose(f);
    }

    ppu_vfs_root = dir;
    strcpy((char*)mem + 0x100, "/dev_hdd1/ps3recomp_fs_aio.bin");
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0 /* O_RDONLY */; ctx.gpr[5] = 0x200;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(0x200);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != 0) { remove(host); return 1; }

    /* 1. The fd's position survives an AIO read. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x40; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = 0x208;
    cellFsLseek(&ctx);
    aio_read(0x300, fd, 0x8000, 0x1000, 0x100);
    CHECK(holds(0x1000, 0x8000, 0x100), "the AIO read returns the words at its own offset");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000; ctx.gpr[5] = 16; ctx.gpr[6] = 0x210;
    cellFsRead(&ctx);
    CHECK(rd64(0x210) == 16 && holds(0x2000, 0x40, 16),
          "cellFsRead after it continues from the fd's position 0x40 (got word 0x%08X)", rd32(0x2000));

    /* 2. AIO reads racing on the same fd each get their own bytes. */
    const int THREADS = 4, ROUNDS = 2000;
    const uint32_t LEN = 0x400;
    std::vector<int> bad(THREADS, 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; t++)
        ts.emplace_back([&, t] {
            const uint32_t aio = 0x400 + 0x40 * t, buf = 0x10000 + 0x10000 * t;
            uint32_t seed = 0x9E3779B9u * (t + 1);
            for (int r = 0; r < ROUNDS; r++) {
                seed = seed * 1664525u + 1013904223u;
                const uint64_t off = (uint64_t)(seed % (WORDS * 4 - LEN)) & ~3ull;
                aio_read(aio, fd, off, buf, LEN);
                if (!holds(buf, off, LEN)) bad[t]++;
            }
        });
    for (auto& th : ts) th.join();
    int total = 0; for (int b : bad) total += b;
    CHECK(total == 0, "%d racing AIO reads over %d threads, %d returned the wrong bytes",
          THREADS * ROUNDS, THREADS, total);

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
