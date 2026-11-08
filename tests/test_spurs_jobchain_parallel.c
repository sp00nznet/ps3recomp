/* Job chains through the real libs/spurs/cellSpurs.c and runtime/spu/spurs_job.c.
 *
 * A job chain whose jobs no SYNC separates may be worked by up to maxContention
 * SPUs at once (cell/spurs/job_chain.h); SYNC waits for the jobs before it. The
 * chain here is
 *
 *     JOB 0, JOB 1, JOB 2, JOB 3, SYNC, JOB 4, END
 *
 * and every job runs the same registered entry, which records how many jobs are
 * running at the moment it starts and when it starts and ends. Modes:
 *
 *   serial    (SPURS_JC_PARALLEL unset)  never more than one job at a time
 *   parallel  (SPURS_JC_PARALLEL=1)      jobs 0-3 overlap, never more than
 *                                        maxContention, and job 4 starts only
 *                                        after jobs 0-3 have all finished;
 *                                        a chain with maxContention 1 stays serial
 *
 * Both modes also notify one job guard from several threads at once and check
 * that no notify is lost. */
#include <ps3emu/ps3types.h>

#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <time.h>

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
    BIN      = 0x08000u,   /* the job binary every job points at (16 bytes) */
    JOBS     = 0x10000u,   /* job descriptors, 0x100 apart                   */
    CHAIN    = 0x20000u,   /* command stream                                 */
    CHAIN1   = 0x21000u,   /* second stream, for the maxContention 1 chain   */
    JC       = 0x30000u,   /* CellSpursJobChain handles                      */
    JC1      = 0x30100u,
    GUARD    = 0x31000u,
    MARK     = 0x38,       /* descriptor offset holding the job's index      */
    NJOBS    = 5,
    MAX_CONT = 4,
};

static long s_running, s_max_running, s_total;
static double s_start[NJOBS], s_end[NJOBS];

static double now_s(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void nap_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

/* The job: r4 is the LS address of its descriptor (spurs_job.c stages it). */
static void fake_job(spu_context* ctx)
{
    const uint32_t desc_ls = ctx->gpr[4]._u32[0];
    const int k = ctx->ls[desc_ls + MARK];
    const long r = __atomic_add_fetch(&s_running, 1, __ATOMIC_ACQ_REL);
    long m = __atomic_load_n(&s_max_running, __ATOMIC_ACQUIRE);
    while (r > m && !__atomic_compare_exchange_n(&s_max_running, &m, r, 0,
                                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    if (k >= 0 && k < NJOBS) s_start[k] = now_s();
    nap_ms(40);
    if (k >= 0 && k < NJOBS) s_end[k] = now_s();
    __atomic_sub_fetch(&s_running, 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s_total, 1, __ATOMIC_ACQ_REL);
}

static void put64(u32 ea, u64 v) { vm_write32(ea, (u32)(v >> 32)); vm_write32(ea + 4, (u32)v); }

static void build_chain(u32 at)
{
    for (int k = 0; k < NJOBS; k++) {
        const u32 job = JOBS + (u32)k * 0x100u;
        memset(vm_base + job, 0, 0x100);
        put64(job + 0x00, BIN);                    /* eaBinary          */
        vm_write32(job + 0x08, 1u << 16);          /* sizeBinary = 16 B */
        vm_base[job + MARK] = (uint8_t)k;
    }
    u32 pc = at;
    for (int k = 0; k < 4; k++, pc += 8) put64(pc, JOBS + (u64)k * 0x100u);   /* JOB 0-3 */
    put64(pc, 2);                       pc += 8;                              /* SYNC    */
    put64(pc, JOBS + 4u * 0x100u);      pc += 8;                              /* JOB 4   */
    put64(pc, 7u | (15u << 3));                                               /* END     */
}

static void run_chain(u32 jc, u32 at, u32 max_cont)
{
    s_running = s_max_running = s_total = 0;
    memset(s_start, 0, sizeof s_start);
    memset(s_end, 0, sizeof s_end);
    build_chain(at);
    assert(cellSpursCreateJobChain(0, jc, at, 0x100, 4, 0, max_cont, 0) == CELL_OK);
    assert(cellSpursRunJobChain(jc) == CELL_OK);
    for (int i = 0; i < 2000 && __atomic_load_n(&s_total, __ATOMIC_ACQUIRE) < NJOBS; i++) nap_ms(5);
    assert(cellSpursJoinJobChain(jc) == CELL_OK);
}

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

/* Several threads notify one guard at once; every notify must count. */
enum { NOTIFIERS = 4, NOTIFIES = 20000 };
static DWORD WINAPI notifier(LPVOID p)
{
    (void)p;
    for (int i = 0; i < NOTIFIES; i++) cellSpursJobGuardNotify(GUARD);
    return 0;
}

static void check_guard(void)
{
    assert(cellSpursJobGuardInitialize(JC, GUARD, 1u << 30, 1, 1) == CELL_OK);
    HANDLE th[NOTIFIERS];
    for (int i = 0; i < NOTIFIERS; i++) {
        th[i] = CreateThread(NULL, 0, notifier, NULL, 0, NULL);
        assert(th[i]);
    }
    for (int i = 0; i < NOTIFIERS; i++) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
    const int g = jg_find(GUARD);
    assert(g >= 0);
    const u32 got = __atomic_load_n(&s_jobguards[g].pending, __ATOMIC_ACQUIRE);
    if (got != (u32)NOTIFIERS * NOTIFIES)
        FAIL("guard counted %u notifies, %u were sent", got, (u32)NOTIFIERS * NOTIFIES);
}

int main(int argc, char** argv)
{
    const int parallel = argc > 1 && !strcmp(argv[1], "parallel");
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    memset(vm_base + BIN, 0x40, 16);   /* any 16 bytes: the fingerprint is all that matters */
    spu_workload_register(spu_workload_fingerprint(vm_base + BIN, 16), fake_job, "fake job");

    run_chain(JC, CHAIN, MAX_CONT);
    const long most = s_max_running;
    if (s_total != NJOBS) FAIL("%ld of %d jobs ran", s_total, NJOBS);
    if (!parallel) {
        if (s_max_running != 1) FAIL("serial chain ran %ld jobs at once", s_max_running);
    } else {
        if (s_max_running < 2) FAIL("parallel chain never overlapped two jobs");
        if (s_max_running > MAX_CONT) FAIL("%ld jobs at once, maxContention is %d", s_max_running, MAX_CONT);
        for (int k = 0; k < 4; k++)
            if (s_start[4] < s_end[k])
                FAIL("job 4 started %.3f s before job %d ended: SYNC not honoured", s_end[k] - s_start[4], k);

        run_chain(JC1, CHAIN1, 1);
        if (s_total != NJOBS) FAIL("maxContention 1: %ld of %d jobs ran", s_total, NJOBS);
        if (s_max_running != 1) FAIL("maxContention 1 chain ran %ld jobs at once", s_max_running);
    }

    check_guard();
    printf("PASS %s (most jobs of the chain at once: %ld)\n", parallel ? "parallel" : "serial", most);
    return 0;
}
