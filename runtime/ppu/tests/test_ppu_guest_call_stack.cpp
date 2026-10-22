// The guest stack an HLE -> guest callback (ppu_guest_call / ppu_guest_call_ct)
// runs on.
//
// It used to be the fixed EA 0xCFFE0000 on every host thread, so two
// callbacks running at once on two host threads built their frames over each
// other, and a callback nested in another started on top of the outer one's
// frame. Here two host threads are inside a callback at the same time, each
// filling its own frame below its stack pointer, and then each nests a second
// callback that does the same; afterwards every frame must still hold what
// its own callback wrote.
//
// Built by tools/test_ppu_guest_call_stack.py against the real ppu_loader.cpp.
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
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
const func_entry function_table[1] = {{0, nullptr, nullptr}};   // no lifted program: registered by hand below
const uint64_t function_table_count = 0;
uint32_t ps3_hle_count(void) { return 0; }
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
uint64_t ppu_guest_call(uint32_t opd, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t);
}

static const uint32_t OPD = 0x1000, CODE = 0x10000, TOC = 0x8000;
static const uint32_t FRAME = 0x200;          // bytes each callback fills below its sp

static void be32(uint32_t ea, uint32_t v)
{
    v = __builtin_bswap32(v);
    std::memcpy(vm_base + ea, &v, 4);
}
static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    std::memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}

enum { THREADS = 2 };
struct record { uint32_t sp, inner_sp; int outer_ok, inner_ok; };
static record s_rec[THREADS];
static volatile int s_inside, s_inner_inside;

static void wait_for(volatile int* n, int want)
{
    __atomic_add_fetch(n, 1, __ATOMIC_SEQ_CST);
    for (long i = 0; i < 200000000L && __atomic_load_n(n, __ATOMIC_SEQ_CST) < want; i++)
        sched_yield();
}

static void fill(uint32_t sp, uint32_t tag)
{
    for (uint32_t o = 4; o <= FRAME; o += 4) be32(sp - o, tag);
}
static int intact(uint32_t sp, uint32_t tag)
{
    for (uint32_t o = 4; o <= FRAME; o += 4) if (rd32(sp - o) != tag) return 0;
    return 1;
}

// gpr[3] = thread index, gpr[4] = 1 for the outer callback, 0 for the nested
// one, 2 for a churn thread's (it only records its stack pointer).
static uint32_t s_churn_sp;
static void callback(ppu_context* ctx)
{
    if (ctx->gpr[4] == 2) { s_churn_sp = (uint32_t)ctx->gpr[1]; return; }
    const int t = (int)ctx->gpr[3];
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t tag = 0xC0DE0000u | (uint32_t)(t << 8) | (uint32_t)ctx->gpr[4];
    fill(sp, tag);
    if (ctx->gpr[4]) {
        s_rec[t].sp = sp;
        wait_for(&s_inside, THREADS);              // both outer frames are live now
        // Nest a callback, through the {code, toc} entry point this time.
        ppu_guest_call_ct(CODE, TOC, (uint64_t)t, 0, 0, 0, 0, 0, 0, 0);
        s_rec[t].outer_ok = intact(sp, tag);
    } else {
        s_rec[t].inner_sp = sp;
        wait_for(&s_inner_inside, THREADS);        // and both inner ones
        s_rec[t].inner_ok = intact(sp, tag);
    }
}

static void* host_thread(void* arg)
{
    const int t = (int)(intptr_t)arg;
    ppu_guest_call(OPD, (uint64_t)t, 1, 0, 0, 0, 0, 0, 0);
    return nullptr;
}

static void* churn_thread(void*)
{
    ppu_guest_call(OPD, 0, 2, 0, 0, 0, 0, 0, 0);
    return nullptr;
}

int main()
{
    void* p = mmap(nullptr, 0x100010000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 1; }
    vm_base = (uint8_t*)p;
    be32(OPD, CODE);
    be32(OPD + 4, TOC);
    ppu_register_function(CODE, callback);

    // First, more host threads than there are slots start, make a callback
    // and end, one after another, as a title's movie threads do all session.
    // Each gives its slot back, so the first slot comes round again.
    enum { CHURN = 100 };
    uint32_t first_churn_sp = 0, last_churn_sp = 0;
    for (int i = 0; i < CHURN; i++) {
        pthread_t c;
        pthread_create(&c, nullptr, churn_thread, nullptr);
        pthread_join(c, nullptr);
        if (i == 0) first_churn_sp = s_churn_sp;
        last_churn_sp = s_churn_sp;
    }

    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++)
        pthread_create(&th[t], nullptr, host_thread, (void*)(intptr_t)t);
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], nullptr);

    unsigned fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        fails += !ok;
    };
    for (int t = 0; t < THREADS; t++)
        std::printf("thread %d: callback sp 0x%08X, nested sp 0x%08X\n", t, s_rec[t].sp, s_rec[t].inner_sp);
    check(first_churn_sp && last_churn_sp == first_churn_sp,
          "a host thread that ends gives its callback stack back (100 threads, one at a time)");
    check(s_rec[0].sp != s_rec[1].sp,
          "after them, two host threads' callbacks still get different stacks");
    check(s_rec[0].outer_ok && s_rec[1].outer_ok,
          "each callback's frame survives the other thread's callback and its own nested one");
    check(s_rec[0].inner_ok && s_rec[1].inner_ok, "each nested callback's frame survives the other thread's");
    check(s_rec[0].inner_sp + FRAME <= s_rec[0].sp && s_rec[1].inner_sp + FRAME <= s_rec[1].sp,
          "a nested callback starts below the frame of the one it is nested in");
    // The guest stack region (vm.h: VM_STACK_BASE, VM_STACK_REGION). 0xCFFE0000,
    // the old shared address, is below it, inside RSX local memory.
    bool in_region = true;
    for (int t = 0; t < THREADS; t++) {
        in_region &= s_rec[t].sp >= 0xD0000000u && s_rec[t].sp < 0xE0000000u;
        in_region &= s_rec[t].inner_sp >= 0xD0000000u && s_rec[t].inner_sp < 0xE0000000u;
    }
    check(in_region, "callback stacks are in the guest stack region");
    return fails ? 1 : 0;
}
