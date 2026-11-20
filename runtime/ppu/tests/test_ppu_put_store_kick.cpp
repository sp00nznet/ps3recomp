// A guest store of the gcm put word reaches GCM (cellGcm_put_on_store), from
// lifted code's inline stores and from the runtime's out-of-line vm_write32;
// a store anywhere else does not. Real lifter output (one `stw r4,0(r3)`
// function, lifted by tools/test_ppu_put_store_kick.py) and the real
// ppu_loader.cpp; GCM is stood in for by a counter. No game code.
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include "win32_compat.h"
extern "C" {
uint8_t* vm_base;
void ps3_stdio_init(void) {}
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
static unsigned s_put_stores;
void cellGcm_put_on_store(void) { ++s_put_stores; }
uint32_t g_gcm_consumer_put_wake_ea = 0;
void cellGcm_fifo_kick_notify(void) { std::abort(); }
void sys_memory_set_image_query(uint64_t (*)(uint64_t, uint64_t)) {}
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { std::abort(); }
extern uint32_t ppu_vm_size;
extern int vm_inline_ok, vm_inline_st_ok;
}

static unsigned failures;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}
static uint32_t guest32(uint32_t ea)
{
    uint32_t v; std::memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v);
}

int main()
{
    const uint32_t size = 0x21000000u;
    vm_base = (uint8_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (vm_base == MAP_FAILED) { std::perror("mmap"); return 1; }
    ppu_vm_size = size;
    check(vm_inline_ok && vm_inline_st_ok, "inline loads and stores are on (no diagnostic armed)");
    const uint32_t put = ppu_hle_inject_base + 0x2000u, get = put + 4u, plain = 0x00100000u;

    ppu_context ctx; std::memset(&ctx, 0, sizeof ctx);   // func_00010000: stw r4,0(r3); blr
    ctx.gpr[3] = put; ctx.gpr[4] = 0x00012340u;
    func_00010000(&ctx);
    check(guest32(put) == 0x00012340u && s_put_stores == 1, "a lifted stw of put lands and reaches GCM");

    ctx.gpr[3] = get; ctx.gpr[4] = 0x55u;
    func_00010000(&ctx);
    ctx.gpr[3] = plain; ctx.gpr[4] = 0x66u;
    func_00010000(&ctx);
    check(guest32(get) == 0x55u && guest32(plain) == 0x66u && s_put_stores == 1,
          "lifted stores to the get word or ordinary memory do not");

    // Parenthesised: the out-of-line function, not the header's inline macro.
    (vm_write32)(put, 0x00012380u);
    (vm_write32)(get, 0x77u);
    check(guest32(put) == 0x00012380u && s_put_stores == 2, "the out-of-line vm_write32 of put reaches GCM, of get does not");

    std::printf("[ppu_put_store_kick] %s (%u failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
