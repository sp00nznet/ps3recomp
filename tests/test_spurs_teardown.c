/* cellSpursShutdownWorkload / WaitForWorkloadShutdown, with manufactured policy
 * modules only.
 *
 * Phase 1: Shutdown stops dispatch; Wait blocks until the in-flight policy
 * returns, then Remove succeeds.
 * Phase 2: Wait is bounded -- a policy that never returns makes it fail with
 * CORE_ERROR_BUSY after SPURS_SHUTDOWN_MS instead of hanging.
 *
 * Also checks the BUSY code a title's retry loop compares (0x8041080A). */
#define spu_workload_dispatch_async fixture_unbounded_dispatch
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"

void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void)
{
    LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    return n.QuadPart * 1000 / f.QuadPart;
}
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t ts, uint32_t id, uint32_t spu, uint32_t tag)
{ (void)ls; (void)ts; (void)id; (void)spu; (void)tag; return 0; }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }
void ydkj_wake_all_event_flags(void) {}
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
int fixture_unbounded_dispatch(const uint8_t* pm, uint32_t size, uint32_t data)
{ (void)pm; (void)size; (void)data; return 0; }

static atomic_uint arrived0, arrived1, gate0, gate1;

static void finish(spu_context* c)
{
    u32 ea = spu_ls_read32(c, 0x1C4), wid = spu_ls_read32(c, 0x1DC);
    vm_write8(ea + 0x80 + wid, 3);
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
    spu_indirect_branch(c);
}
static void hold0(spu_context* c)
{
    atomic_fetch_add(&arrived0, 1);
    while (!atomic_load(&gate0)) Sleep(1);
    finish(c);
}
static void hold1(spu_context* c)
{
    atomic_fetch_add(&arrived1, 1);
    while (!atomic_load(&gate1)) Sleep(1);
    finish(c);
}

static void wait_arrived(atomic_uint* p)
{
    for (unsigned i = 0; i < 5000; i++) { if (atomic_load(p)) return; Sleep(1); }
    assert(!"policy did not start");
}

typedef struct { u64 ea; u32 wid; } wait_arg;
static atomic_int wait_done, wait_rc;
static DWORD WINAPI wait_thread(void* p)
{
    const wait_arg* a = (const wait_arg*)p;
    wait_rc = cellSpursWaitForWorkloadShutdown(a->ea, a->wid);
    atomic_store(&wait_done, 1);
    return 0;
}

int main(void)
{
    setenv("SPURS_SHUTDOWN_MS", "150", 1);
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    memset(vm_base + 0x9000, 0x22, 16);
    memset(vm_base + 0x9100, 0x33, 16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + 0x9000, 16),
                              hold0, 77, "teardown hold 0");
    spu_workload_register_img(spu_workload_fingerprint(vm_base + 0x9100, 16),
                              hold1, 78, "teardown hold 1");

    assert(cellSpursInitialize((CellSpurs*)0x1000, 2, 0, 0, 0) == 0);
    u32* widp = (u32*)(uintptr_t)0x8000;   /* guest out-param, as the HLE expects */
    assert(cellSpursAddWorkload((CellSpurs*)0x1000, widp, (void*)0x9000, 16, 0,
                                NULL, 1, 2) == 0);
    assert(vm_read32(0x8000) == 0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000, widp, (void*)0x9100, 16, 0,
                                NULL, 1, 2) == 0);
    assert(vm_read32(0x8000) == 1);

    vm_base[0x1000 + SPURS_WKL_READY1 + 0] = 1;
    wait_arrived(&arrived0);

    /* ---- phase 1: drain an in-flight policy ---- */
    assert(cellSpursShutdownWorkload(0x1000, 0) == CELL_OK);
    assert(vm_base[0x1000 + SPURS_WKL_STATE1 + 0] != 2);
    assert((vm_read32(0x1000 + SPURS_WKL_ENABLED) & 0x80000000u) == 0);

    wait_arg a0 = { 0x1000, 0 };
    HANDLE h = CreateThread(NULL, 0, wait_thread, &a0, 0, NULL);
    Sleep(50);
    assert(!atomic_load(&wait_done));       /* the policy is still inside */
    assert((u32)cellSpursRemoveWorkload((CellSpurs*)0x1000, 0) == 0x8041080Au);
    atomic_store(&gate0, 1);
    assert(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CloseHandle(h);
    assert(wait_rc == CELL_OK);
    assert(cellSpursRemoveWorkload((CellSpurs*)0x1000, 0) == CELL_OK);
    puts("PASS: Shutdown stops dispatch, Wait drains, Remove succeeds");

    /* ---- phase 2: Wait is bounded ---- */
    vm_base[0x1000 + SPURS_WKL_READY1 + 1] = 1;
    wait_arrived(&arrived1);
    assert(cellSpursShutdownWorkload(0x1000, 1) == CELL_OK);
    const unsigned long long t0 = ps3_ms_now();
    const s32 rc = cellSpursWaitForWorkloadShutdown(0x1000, 1);
    const unsigned long long dt = ps3_ms_now() - t0;
    assert((u32)rc == 0x8041080Au);
    assert(dt >= 100 && dt < 2000);
    atomic_store(&gate1, 1);
    assert(cellSpursWaitForWorkloadShutdown(0x1000, 1) == CELL_OK);
    assert(cellSpursRemoveWorkload((CellSpurs*)0x1000, 1) == CELL_OK);
    puts("PASS: Wait is bounded and reports the 0x8041080A BUSY");

    assert(cellSpursFinalize((CellSpurs*)0x1000) == 0);
    free(vm_base);
    return 0;
}
