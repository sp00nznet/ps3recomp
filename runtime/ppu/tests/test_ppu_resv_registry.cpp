// The reservation registry keeps one entry per distinct ppu_context.
//
// A guest thread that starts in a reused ppu_thread_info slot registers the
// same ctx again. Each case below registers one context many times and then
// checks that a context registered AFTER that is still reached by the three
// reservation breaks: a store-conditional (ppu_stwcx32 -> ppu_resv_break), a
// plain store (ppu_resv_break_store) and a write to the line
// (ppu_resv_break_line). A victim left reserved is a stwcx. that would commit
// over another thread's update.
//
// The registry is process-global and has no reset, so every case is its own
// process: tools/test_ppu_resv_registry.py runs each mode below in turn.
//   baseline    one registration per context (passes with or without the fix)
//   churn       256 registrations of one reused context before the victim
//   concurrent  8 threads: 256 registrations of a shared ctx + one victim each
//   capacity    128 distinct contexts, registered three times, all reachable
//   overflow    a 129th distinct context must stop the process with a message
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
const func_entry function_table[1] = {{0, nullptr, nullptr}};
const uint64_t function_table_count = 0;
uint32_t ps3_hle_count(void) { return 0; }
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_resv_register(ppu_context* c);
void ppu_resv_break_store(uint64_t ea);
void ppu_resv_break_line(uint32_t ea);
int ppu_stwcx32(uint64_t ea, uint32_t expected, uint32_t val);
}

enum { CAPACITY = 128, REPEATS = 256, THREADS = 8 };
static const uint64_t INVALID = 0x100000000ull;   // PPU_RESV_INVALID
static const uint32_t RESV_LINE = 0x1800, RESV_WORD = RESV_LINE + 4;

// Registered contexts must outlive the process: the registry keeps them.
static ppu_context writer, reused, victim, unrelated;
static ppu_context victims[THREADS];
static ppu_context many[CAPACITY + 1];

// Break every reservation on RESV_WORD the way `how` does, as `writer`.
static void break_as_writer(const char* how)
{
    g_active_ctx = &writer;
    if (!std::strcmp(how, "stwcx")) {
        writer.reserve_addr = RESV_WORD;
        if (!ppu_stwcx32(RESV_WORD, 0, 1)) { std::printf("FAIL: writer's own stwcx did not commit\n"); std::exit(1); }
    } else if (!std::strcmp(how, "store")) {
        ppu_resv_break_store(RESV_WORD);
    } else {
        ppu_resv_break_line(RESV_WORD);
    }
}

static int churn_case(unsigned repeats)
{
    ppu_resv_register(&writer);
    ppu_resv_register(&unrelated);
    for (unsigned n = 0; n < repeats; n++) ppu_resv_register(&reused);
    ppu_resv_register(&victim);
    int fails = 0;
    for (const char* how : {"stwcx", "store", "line"}) {
        std::memset(vm_base + RESV_LINE, 0, 128);
        victim.reserve_addr = RESV_WORD;
        unrelated.reserve_addr = RESV_LINE + 128;   // another line: must survive
        reused.reserve_addr = INVALID;
        break_as_writer(how);
        const bool ok = victim.reserve_addr == INVALID && unrelated.reserve_addr == RESV_LINE + 128;
        std::printf("[%s] %s break after %u registrations of one reused context: victim %s\n",
                    ok ? "PASS" : "FAIL", how, repeats,
                    victim.reserve_addr == INVALID ? "invalidated" : "STILL RESERVED");
        fails += !ok;
    }
    return fails;
}

static int concurrent_case()
{
    ppu_resv_register(&writer);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < THREADS; i++) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned n = 0; n < REPEATS; n++) ppu_resv_register(&reused);
            ppu_resv_register(&victims[i]);
        });
    }
    while (ready.load() != THREADS) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    for (auto& v : victims) v.reserve_addr = RESV_WORD;
    break_as_writer("line");
    unsigned broken = 0;
    for (auto& v : victims) broken += v.reserve_addr == INVALID;
    std::printf("[%s] concurrent duplicate registration: %u of %d victims invalidated\n",
                broken == THREADS ? "PASS" : "FAIL", broken, THREADS);
    return broken != THREADS;
}

static int capacity_case()
{
    for (unsigned r = 0; r < 3; r++)
        for (unsigned i = 0; i < CAPACITY; i++) ppu_resv_register(&many[i]);
    for (unsigned i = 0; i < CAPACITY; i++) many[i].reserve_addr = RESV_WORD;
    g_active_ctx = nullptr;
    ppu_resv_break_line(RESV_WORD);
    unsigned broken = 0;
    for (unsigned i = 0; i < CAPACITY; i++) broken += many[i].reserve_addr == INVALID;
    std::printf("[%s] %d distinct contexts registered three times: %u invalidated\n",
                broken == CAPACITY ? "PASS" : "FAIL", CAPACITY, broken);
    return broken != CAPACITY;
}

static int overflow_case()
{
    for (unsigned i = 0; i <= CAPACITY; i++) ppu_resv_register(&many[i]);
    std::printf("FAIL: a %dth distinct context was accepted without a slot\n", CAPACITY + 1);
    return 1;
}

int main(int argc, char** argv)
{
    vm_base = (uint8_t*)std::calloc(1, 0x10000);
    const char* mode = argc > 1 ? argv[1] : "";
    if (!std::strcmp(mode, "baseline"))   return churn_case(1) ? 1 : 0;
    if (!std::strcmp(mode, "churn"))      return churn_case(REPEATS) ? 1 : 0;
    if (!std::strcmp(mode, "concurrent")) return concurrent_case() ? 1 : 0;
    if (!std::strcmp(mode, "capacity"))   return capacity_case() ? 1 : 0;
    if (!std::strcmp(mode, "overflow"))   return overflow_case();
    std::fprintf(stderr, "usage: %s baseline|churn|concurrent|capacity|overflow\n", argv[0]);
    return 2;
}
