/*
 * ps3recomp - cellFs imports registered by ppu_fs_register().
 *
 * A firmware import with no registered handler takes the unresolved-NID path:
 * CELL_OK with every out-parameter left unwritten. This fixture compiles
 * ppu_fs.cpp in, records each ps3_hle_register_ctx() call, and checks four
 * imports a title can call during boot:
 *   - each is registered under the NID a title imports it by;
 *   - cellFsGetFreeSize writes a block size and a non-zero free block count
 *     (both big-endian) and returns CELL_OK;
 *   - cellFsChmod and cellFsUtime return CELL_OK;
 *   - cellFsSdataOpenByFd returns CELL_ENOSYS instead of a silent CELL_OK.
 *
 * Driven by tools/test_ppu_fs_imports.py (writes the stub ppu_recomp.h).
 */
#include "ppu_fs.cpp"
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
}
/* ps3_dev_flash_path (runtime/platform), which host_path calls for /dev_flash: the real one, it needs nothing. */
#include "../../platform/ps3_dev_flash.c"
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

struct Reg { uint32_t nid; const char* name; void (*fn)(ppu_context*); };
static std::vector<Reg> g_regs;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    g_regs.push_back({nid, name, fn});
}

static void (*find(uint32_t nid))(ppu_context*)
{
    for (auto& r : g_regs) if (r.nid == nid) return r.fn;
    return nullptr;
}
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

int main()
{
    static uint8_t mem[1 << 16];
    vm_base = mem; ppu_vm_size = sizeof mem;
    ppu_fs_register();

    /* NIDs as they appear in a title's cellFs import table. */
    const uint32_t NID_GETFREESIZE = 0xAA3B4BCDu, NID_CHMOD = 0x99406D0Bu,
                   NID_UTIME = 0xBEF554A4u, NID_SDATAOPENBYFD = 0x6D3BB15Bu;
    struct { uint32_t nid; const char* name; } want[] = {
        {NID_GETFREESIZE, "cellFsGetFreeSize"}, {NID_CHMOD, "cellFsChmod"},
        {NID_UTIME, "cellFsUtime"},             {NID_SDATAOPENBYFD, "cellFsSdataOpenByFd"},
    };
    for (auto& w : want) {
        CHECK(ps3_compute_nid(w.name) == w.nid, "%s: NID 0x%08X is the hash of its name", w.name, w.nid);
        CHECK(find(w.nid) != nullptr, "%s: handler registered", w.name);
    }

    ppu_context ctx;
    if (auto f = find(NID_GETFREESIZE)) {   /* (path, u32* block_size, u64* free_block_count) */
        memset(&ctx, 0, sizeof ctx);
        strcpy((char*)mem + 0x100, "/dev_hdd0");
        memset(mem + 0x200, 0xEE, 16);
        ctx.gpr[3] = 0x100; ctx.gpr[4] = 0x200; ctx.gpr[5] = 0x208;
        f(&ctx);
        uint32_t bs = rd32(0x200); uint64_t fb = rd64(0x208);
        CHECK(ctx.gpr[3] == 0, "cellFsGetFreeSize returns CELL_OK");
        CHECK(bs == 4096, "cellFsGetFreeSize block_size = %u", bs);
        CHECK(fb != 0 && fb != 0xEEEEEEEEEEEEEEEEull, "cellFsGetFreeSize free_block_count = %llu",
              (unsigned long long)fb);
    }
    struct { uint32_t nid; const char* name; uint32_t ret; } rets[] = {
        {NID_CHMOD, "cellFsChmod", 0}, {NID_UTIME, "cellFsUtime", 0},
        {NID_SDATAOPENBYFD, "cellFsSdataOpenByFd", 0x80010003u /* CELL_ENOSYS */}};
    for (auto& r : rets) if (auto f = find(r.nid)) {
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100;
        f(&ctx);
        CHECK((uint32_t)ctx.gpr[3] == r.ret, "%s returns 0x%08X (want 0x%08X)", r.name, (uint32_t)ctx.gpr[3], r.ret);
    }
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
