/* The SPU number a job chain's jobs see follows the chain's priority table.
 *
 * Runs chains through the real libs/spurs/cellSpurs.c and runtime/spu/spurs_job.c.
 * Each chain is one JOB and END; the job is a "JOBCRT Ver13" binary, so
 * spurs_job.c stages the SPURS kernel's view in its local store, and the job
 * records the SPU number its crt would read there (LS 0x1C8,
 * cellSpursGetCurrentSpuId). A priority of 0 keeps a chain off that SPU, so a
 * job must never report one of those:
 *
 *   cellSpursCreateJobChain, table 00 06 06 06 00 00 00 00   -> SPU 1
 *   CreateJobChainWithAttribute, table 00 00 00 00 02 00 00 00 -> SPU 4
 *   cellSpursCreateJobChain, table 08 08 08 08 00 00 00 00   -> SPU 0
 *   cellSpursCreateJobChain, no table                        -> SPU 0
 */
#include <ps3emu/ps3types.h>

#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <time.h>
void (*g_rsx_main_release_hook)(uint32_t line);   /* libs/video/rsx_commands.c (#729) */

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x100000;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
/* Job completions are pushed to the chain's event queues; none is attached here. */
int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                               uint64_t data1, uint64_t data2, uint64_t data3)
{ (void)queue_id; (void)source; (void)data1; (void)data2; (void)data3; return 0; }
/* Referenced from spu_workload.c and spu_channels.c paths this test never takes
 * (task and policy-module dispatch); the same stubs test_spu_dispatch_miss.c uses. */
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset_ea, uint32_t taskId,
                                uint32_t spuNum, uint32_t dmaTagId)
{ (void)ls; (void)taskset_ea; (void)taskId; (void)spuNum; (void)dmaTagId; return 0; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}

enum {
    BIN      = 0x08000u,   /* the job binary (0x40 bytes, "JOBCRT Ver13" at +0x30) */
    BIN_SIZE = 0x40u,
    JOB      = 0x10000u,   /* the job descriptor                                   */
    CHAIN    = 0x20000u,   /* command streams, 0x100 apart                         */
    JC       = 0x30000u,   /* CellSpursJobChain handles, 0x100 apart               */
    PRIO     = 0x31000u,   /* priority tables, 0x10 apart                          */
    ATTR     = 0x32000u,   /* CellSpursJobChainAttribute                           */
    SPURS    = 0x33000u,   /* any non-zero CellSpurs EA                            */
};

static volatile long s_ran;
static volatile u32 s_seen;

static void nap_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

/* The job: what cellSpursGetCurrentSpuId returns to it (LS 0x1C8, big-endian). */
static void fake_job(spu_context* ctx)
{
    const uint8_t* q = ctx->ls + 0x1C8;
    s_seen = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
    __atomic_add_fetch(&s_ran, 1, __ATOMIC_ACQ_REL);
}

static void put64(u32 ea, u64 v) { vm_write32(ea, (u32)(v >> 32)); vm_write32(ea + 4, (u32)v); }

static u32 put_prio(int k, const u8 table[8])
{
    const u32 ea = PRIO + (u32)k * 0x10u;
    memcpy(vm_base + ea, table, 8);
    return ea;
}

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

/* One JOB, then END. Returns the SPU number the job saw. */
static u32 run_one(int k, int with_attribute, u32 prio_ea)
{
    const u32 jc = JC + (u32)k * 0x100u, at = CHAIN + (u32)k * 0x100u;
    put64(at, JOB);
    put64(at + 8, 7u | (15u << 3));                          /* END */
    s_ran = 0;
    s_seen = 0xFFFFFFFFu;
    if (with_attribute) {
        assert(_cellSpursJobChainAttributeInitialize(0, 0, ATTR, at, 0x100, 1, prio_ea, 1) == CELL_OK);
        assert(cellSpursCreateJobChainWithAttribute(SPURS, jc, ATTR) == CELL_OK);
    } else {
        assert(cellSpursCreateJobChain(0, jc, at, 0x100, 1, prio_ea, 1, 0) == CELL_OK);
    }
    assert(cellSpursRunJobChain(jc) == CELL_OK);
    for (int i = 0; i < 2000 && !__atomic_load_n(&s_ran, __ATOMIC_ACQUIRE); i++) nap_ms(5);
    assert(cellSpursJoinJobChain(jc) == CELL_OK);
    if (!s_ran) FAIL("chain %d: the job never ran", k);
    return s_seen;
}

int main(void)
{
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    memset(vm_base + BIN, 0x40, BIN_SIZE);
    memcpy(vm_base + BIN + 0x30, "JOBCRT Ver13", 12);
    spu_workload_register(spu_workload_fingerprint(vm_base + BIN, BIN_SIZE), fake_job, "fake job");
    memset(vm_base + JOB, 0, 0x100);
    put64(JOB + 0x00, BIN);                                  /* eaBinary   */
    vm_write32(JOB + 0x08, (BIN_SIZE >> 4) << 16);           /* sizeBinary */

    static const u8 edge[8]  = { 0, 6, 6, 6, 0, 0, 0, 0 };
    static const u8 spu4[8]  = { 0, 0, 0, 0, 2, 0, 0, 0 };
    static const u8 first[8] = { 8, 8, 8, 8, 0, 0, 0, 0 };
    u32 got;
    if ((got = run_one(0, 0, put_prio(0, edge))) != 1)
        FAIL("table 00 06 06 06 00 00 00 00: job saw SPU %u, expected 1", got);
    if ((got = run_one(1, 1, put_prio(1, spu4))) != 4)
        FAIL("attribute table 00 00 00 00 02 00 00 00: job saw SPU %u, expected 4", got);
    if ((got = run_one(2, 0, put_prio(2, first))) != 0)
        FAIL("table 08 08 08 08 00 00 00 00: job saw SPU %u, expected 0", got);
    if ((got = run_one(3, 0, 0)) != 0)
        FAIL("no table: job saw SPU %u, expected 0", got);
    printf("OK: job chains' jobs see an SPU their priority table allows\n");
    return 0;
}

/* Queue lifetime is mocked in this scheduler/table fixture. The routing
 * fixture links real LV2 queues and checks generation changes on recreation. */
uint64_t sys_event_queue_generation(uint32_t id) { return id ? 1 : 0; }
void sys_event_queue_cancel_generation(uint32_t id, uint64_t generation)
{ if (generation) sys_event_queue_cancel_by_id(id); }
void sys_event_queue_uncancel_generation(uint32_t id, uint64_t generation)
{ if (generation) sys_event_queue_uncancel_by_id(id); }
uint32_t sys_event_queue_push_generation(uint32_t id, uint64_t generation,
    uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)generation;
    return sys_event_queue_push_by_id(id, source, d1, d2, d3) == 0 ? CELL_OK : CELL_EBUSY;
}
