// ps3_indirect_call's recursion guard with host coroutines that end inside a call.
//
// 5000 coroutines (ucontext) each run a guest call chain five ps3_indirect_call
// levels deep and, at the bottom, switch back to the home context without
// returning -- the way a coroutine that runs guest jobs ends a finished job.
// Around each switch the home side does what a coroutine layer does with
// ps3_ppu_dispatch_depth_slot(): save the count before entering the coroutine
// and restore it when control comes back. Afterwards an indirect call to a
// leaf function on the home thread must still run.
//
// Built by tools/test_ppu_dispatch_depth.py against the real ppu_loader.cpp.
// macOS keeps the ucontext routines behind _XOPEN_SOURCE (deprecated, still
// present); _DARWIN_C_SOURCE keeps the rest of the system headers visible.
// Both must come before the first system header.
#if defined(__APPLE__)
#  define _XOPEN_SOURCE 600
#  define _DARWIN_C_SOURCE
#endif
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ucontext.h>
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
const func_entry function_table[1] = {{0, nullptr, nullptr}};   // no lifted program: registered by hand below
const uint64_t function_table_count = 0;
uint32_t ps3_hle_count(void) { return 0; }
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
void ps3_indirect_call(ppu_context* ctx);
// The coroutine hook this test exercises. Weak, so the same test also builds
// against a loader without it and shows what happens there.
__attribute__((weak)) int* ps3_ppu_dispatch_depth_slot(void);
__attribute__((weak)) int* ps3_ppu_vcall_sp_slot(void);
}

enum { LEVELS = 5, COROUTINES = 5000, STACK = 256 * 1024 };
static const uint32_t CHAIN = 0x10000, LEAF = 0x20000;

static ucontext_t s_home, s_co;
static unsigned s_bottoms, s_leaf_calls;

static void chain(ppu_context* ctx)
{
    if (ctx->gpr[3] + 1 < LEVELS) {
        ctx->gpr[3]++;
        ctx->ctr = CHAIN;
        ps3_indirect_call(ctx);
        return;
    }
    ++s_bottoms;
    swapcontext(&s_co, &s_home);   // the coroutine ends here; it is never resumed
}

static void leaf(ppu_context*) { ++s_leaf_calls; }

static ppu_context s_ctx;
static void coroutine_main(void)
{
    s_ctx.gpr[3] = 0;
    s_ctx.ctr = CHAIN;
    ps3_indirect_call(&s_ctx);
}

int main()
{
    vm_base = (uint8_t*)std::calloc(1, 0x100000);
    ppu_register_function(CHAIN, chain);
    ppu_register_function(LEAF, leaf);
    const bool hook = ps3_ppu_dispatch_depth_slot && ps3_ppu_vcall_sp_slot;
    std::printf("coroutine hook ps3_ppu_dispatch_depth_slot: %s\n", hook ? "present" : "absent");

    static char stack[STACK];
    int depth0 = hook ? *ps3_ppu_dispatch_depth_slot() : 0;
    int vsp0 = hook ? *ps3_ppu_vcall_sp_slot() : 0;
    for (int i = 0; i < COROUTINES; i++) {
        int saved_depth = 0, saved_vsp = 0;
        if (hook) { saved_depth = *ps3_ppu_dispatch_depth_slot(); saved_vsp = *ps3_ppu_vcall_sp_slot(); }
        getcontext(&s_co);
        s_co.uc_stack.ss_sp = stack;
        s_co.uc_stack.ss_size = sizeof stack;
        s_co.uc_link = &s_home;
        makecontext(&s_co, coroutine_main, 0);
        swapcontext(&s_home, &s_co);
        if (hook) { *ps3_ppu_dispatch_depth_slot() = saved_depth; *ps3_ppu_vcall_sp_slot() = saved_vsp; }
    }

    unsigned fails = 0;
    std::printf("[%s] %u of %d coroutines reached the bottom of their %d-level chain\n",
                s_bottoms == COROUTINES ? "PASS" : "FAIL", s_bottoms, COROUTINES, LEVELS);
    fails += s_bottoms != COROUTINES;
    if (hook) {
        int d = *ps3_ppu_dispatch_depth_slot(), v = *ps3_ppu_vcall_sp_slot();
        std::printf("[%s] home thread depth %d -> %d, vcall sp %d -> %d\n",
                    d == depth0 && v == vsp0 ? "PASS" : "FAIL", depth0, d, vsp0, v);
        fails += d != depth0 || v != vsp0;
    }
    s_ctx.ctr = LEAF;
    ps3_indirect_call(&s_ctx);
    std::printf("[%s] indirect call to a leaf on the home thread afterwards: %s\n",
                s_leaf_calls == 1 ? "PASS" : "FAIL", s_leaf_calls == 1 ? "ran" : "skipped");
    fails += s_leaf_calls != 1;
    return fails ? 1 : 0;
}
