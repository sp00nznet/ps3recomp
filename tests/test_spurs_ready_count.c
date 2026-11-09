/* SPURS_IDLE_PROBE=0, real-kernel admission: the kernel does not spend a
 * workload's ready count on admission. The count is the number of SPUs the
 * workload asks for, it stays runnable while the count exceeds its contention,
 * and its own policy module stores the count back to zero when it runs out of
 * work; no idle probe admits it. Manufactured policy modules on the real
 * scheduler; no game code. */
#define spu_workload_dispatch_async fixture_unbounded_dispatch
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#include <assert.h>
#include <stdatomic.h>

void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart * 1000 / f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t ts, uint32_t id, uint32_t spu, uint32_t tag)
{ (void)ls; (void)ts; (void)id; (void)spu; (void)tag; return 0; }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }
void ydkj_wake_all_event_flags(void) {}
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
int fixture_unbounded_dispatch(const uint8_t* pm, uint32_t size, uint32_t data)
{ (void)pm; (void)size; (void)data; assert(!"unbounded dispatch"); return 0; }

#define SPURS ((CellSpurs*)0x1000)
#define SPURS_EA 0x1000u
#define JOBS_PER_KICK 3u

static atomic_uint runs, held, peak, gate;

static void exit_to_kernel(spu_context* c) { c->pc = SPURS_PM_EXIT_TO_KERNEL_LS; spu_indirect_branch(c); }

/* A job manager with JOBS_PER_KICK jobs, one per run: the run that takes the
 * last one stores the ready count back to zero, as a drained WWS list does. */
static void drain_policy(spu_context* c)
{
    u32 ea = spu_ls_read32(c, 0x1C4), wid = spu_ls_read32(c, 0x1DC);
    if (atomic_fetch_add(&runs, 1) + 1 == JOBS_PER_KICK) {
        spu_lockline_lock();
        vm_base[ea + wkl_ready_off(wid)] = 0;
        ef_line_end(ea);
    }
    exit_to_kernel(c);
}

/* Holds its SPU until the gate opens; counts how many run at once. */
static void held_policy(spu_context* c)
{
    unsigned n = atomic_fetch_add(&held, 1) + 1, p = atomic_load(&peak);
    while (p < n && !atomic_compare_exchange_weak(&peak, &p, n)) {}
    while (!atomic_load(&gate)) Sleep(1);
    atomic_fetch_sub(&held, 1);
    u32 ea = spu_ls_read32(c, 0x1C4), wid = spu_ls_read32(c, 0x1DC);
    vm_write8(ea + 0x80 + wid, 3);   /* leaves the runnable set */
    exit_to_kernel(c);
}

static void wait_for(atomic_uint* v, unsigned want, const char* what)
{
    for (unsigned i = 0; i < 3000; i++) { if (atomic_load(v) >= want) return; Sleep(1); }
    fprintf(stderr, "FAIL: %s (%u < %u)\n", what, atomic_load(v), want);
    exit(1);
}

static void finalize(void)
{
    for (unsigned i = 0; i < 3000; i++) {
        s32 rc = cellSpursFinalize(SPURS);
        if (!rc) return;
        assert((u32)rc == CELL_SPURS_CORE_ERROR_BUSY);
        Sleep(1);
    }
    assert(!"finalize");
}

static void register_pm(unsigned ea, unsigned byte, spu_lifted_entry_fn fn, int image)
{
    memset(vm_base + ea, byte, 16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + ea, 16), fn, image, "ready count fixture");
}

int main(void)
{
    setenv("SPURS_IDLE_PROBE", "0", 1);
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    register_pm(0x9000, 0x11, drain_policy, 90);
    register_pm(0x9100, 0x22, held_policy, 91);

    /* No ready count, no signal: never admitted. */
    assert(cellSpursInitialize(SPURS, 1, 0, 0, 0) == 0);
    assert(cellSpursAddWorkload(SPURS, (u32*)0x8000, (void*)0x9000, 16, 0, NULL, 1, 1) == 0);
    Sleep(30);
    assert(atomic_load(&runs) == 0);

    /* One store of 1 serves every job: the count survives each admission until
     * the module itself clears it, and then nothing runs it again. */
    assert(cellSpursReadyCountStore(SPURS, 0, 1) == 0);
    wait_for(&runs, JOBS_PER_KICK, "ready count spent on the first admission");
    Sleep(30);
    assert(atomic_load(&runs) == JOBS_PER_KICK);
    assert(vm_read8(SPURS_EA + SPURS_WKL_READY1) == 0);
    assert(vm_read8(SPURS_EA + SPURS_WKL_CURCONT) == 0);
    vm_write8(SPURS_EA + 0x80, 3);
    finalize();
    puts("SPURS ready count persists across admissions until the policy module clears it PASS");

    /* The count caps contention: 1 admits one SPU of two, 2 admits the second. */
    assert(cellSpursInitialize(SPURS, 2, 0, 0, 0) == 0);
    assert(cellSpursAddWorkload(SPURS, (u32*)0x8000, (void*)0x9100, 16, 0, NULL, 1, 2) == 0);
    assert(cellSpursReadyCountStore(SPURS, 0, 1) == 0);
    wait_for(&held, 1, "first SPU");
    Sleep(30);
    assert(atomic_load(&peak) == 1 && vm_read8(SPURS_EA + SPURS_WKL_CURCONT) == 1);
    assert(vm_read8(SPURS_EA + SPURS_WKL_READY1) == 1);
    assert(cellSpursReadyCountStore(SPURS, 0, 2) == 0);
    wait_for(&held, 2, "second SPU");
    atomic_store(&gate, 1);
    finalize();
    puts("SPURS ready count bounds the SPUs a workload gets PASS");
    free(vm_base);
    return 0;
}
