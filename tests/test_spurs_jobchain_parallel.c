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
 *                                        a chain with maxContention 1 stays serial;
 *                                        jobs claim their SPU number in chain
 *                                        order, also when they had to wait for one;
 *                                        a chain whose priority table allows SPUs
 *                                        1-3 runs its jobs only as SPUs 1-3, at
 *                                        most three at once
 *
 * In both modes job 0 runs longest, and the completions must still be announced
 * in chain order (data2 = 0, 1, 2, ...): on hardware each SPU takes the chain's
 * jobs in order, and a consumer that reads data2 as "done up to here" must not
 * hear of job 1 while job 0 is still writing. Both modes also notify one job
 * guard from several threads at once and check that no notify is lost. */
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
/* Job completions are pushed to the chain's event queues: one is attached in
 * main, and every push is recorded in order (data1 = chain, data2 = job). */
enum { MAX_PUSHES = 256 };
static struct { u32 jc; int job; } s_push[MAX_PUSHES];
static long s_npush;
int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                               uint64_t data1, uint64_t data2, uint64_t data3)
{
    (void)queue_id; (void)source; (void)data3;
    const long i = __atomic_fetch_add(&s_npush, 1, __ATOMIC_ACQ_REL);
    if (i < MAX_PUSHES) { s_push[i].jc = (u32)data1; s_push[i].job = (int)data2; }
    return 0;
}
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
    CHAIN2   = 0x22000u,   /* third stream, for the claim order check        */
    CHAIN3   = 0x23000u,   /* fourth stream, for the priority table check    */
    JC       = 0x30000u,   /* CellSpursJobChain handles                      */
    JC1      = 0x30100u,
    JC2      = 0x30200u,
    JC3      = 0x30300u,
    GUARD    = 0x31000u,
    PRIO     = 0x32000u,   /* the priority table of the JC3 chain            */
    MARK     = 0x38,       /* descriptor offset holding the job's index      */
    NJOBS    = 5,
    MAX_CONT = 4,
    NSPUS    = CELL_SPURS_MAX_SPU,   /* SPUs of a chain created with no instance */
    MAXJ     = NSPUS,                /* job descriptors built                    */
};

extern SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;   /* spurs_job.c */

static long s_running, s_max_running, s_total;
static double s_start[MAXJ], s_end[MAXJ];
static unsigned s_dur_ms[MAXJ];   /* how long each job runs */
static int s_spu[MAXJ];           /* the SPU number each job ran as */

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
    if (k >= 0 && k < MAXJ) {
        s_spu[k] = (int)g_spurs_job_spu_num;
        s_start[k] = now_s();
        nap_ms(s_dur_ms[k]);
        s_end[k] = now_s();
    }
    __atomic_sub_fetch(&s_running, 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s_total, 1, __ATOMIC_ACQ_REL);
}

static void put64(u32 ea, u64 v) { vm_write32(ea, (u32)(v >> 32)); vm_write32(ea + 4, (u32)v); }

static void build_jobs(void)
{
    for (int k = 0; k < MAXJ; k++) {
        const u32 job = JOBS + (u32)k * 0x100u;
        memset(vm_base + job, 0, 0x100);
        put64(job + 0x00, BIN);                    /* eaBinary          */
        vm_write32(job + 0x08, 1u << 16);          /* sizeBinary = 16 B */
        vm_base[job + MARK] = (uint8_t)k;
    }
}

static void build_chain(u32 at)
{
    u32 pc = at;
    for (int k = 0; k < 4; k++, pc += 8) put64(pc, JOBS + (u64)k * 0x100u);   /* JOB 0-3 */
    put64(pc, 2);                       pc += 8;                              /* SYNC    */
    put64(pc, JOBS + 4u * 0x100u);      pc += 8;                              /* JOB 4   */
    put64(pc, 7u | (15u << 3));                                               /* END     */
}

static void start_chain(u32 jc, u32 at, u32 max_cont)
{
    s_running = s_max_running = s_total = 0;
    memset(s_start, 0, sizeof s_start);
    memset(s_end, 0, sizeof s_end);
    for (int k = 0; k < MAXJ; k++) s_spu[k] = -1;
    assert(cellSpursCreateJobChain(0, jc, at, 0x100, 4, 0, max_cont, 0) == CELL_OK);
    assert(cellSpursRunJobChain(jc) == CELL_OK);
}

static void finish_chain(u32 jc, int njobs)
{
    for (int i = 0; i < 2000 && __atomic_load_n(&s_total, __ATOMIC_ACQUIRE) < njobs; i++) nap_ms(5);
    assert(cellSpursJoinJobChain(jc) == CELL_OK);
}

static void run_chain(u32 jc, u32 at, u32 max_cont)
{
    build_chain(at);
    start_chain(jc, at, max_cont);
    finish_chain(jc, NJOBS);
}

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

/* The first njobs completions pushed for chain jc name jobs 0, 1, 2, ... */
static void check_done_order(u32 jc, int njobs)
{
    long n = __atomic_load_n(&s_npush, __ATOMIC_ACQUIRE);
    if (n > MAX_PUSHES) n = MAX_PUSHES;
    int next = 0;
    char seen[64] = "";
    for (long i = 0; i < n && next < njobs; i++) {
        if (s_push[i].jc != jc) continue;
        const size_t l = strlen(seen);
        snprintf(seen + l, sizeof seen - l, " %d", s_push[i].job);
        if (s_push[i].job != next)
            FAIL("chain 0x%X: completions announced as%s..., expected job %d next",
                 jc, seen, next);
        next++;
    }
    if (next < njobs) FAIL("chain 0x%X: %d of %d completions announced", jc, next, njobs);
}

/* Every SPU number is held when the chain starts, so all of its jobs queue for
 * one; the numbers are then released one at a time, lowest first, slower than
 * a job claims one and faster than it finishes. Taking the chain's jobs in
 * order, job k must get SPU k. */
static void check_claim_order(void)
{
    u32 pc = CHAIN2;
    for (int k = 0; k < NSPUS; k++, pc += 8) put64(pc, JOBS + (u64)k * 0x100u);
    put64(pc, 7u | (15u << 3));                                               /* END */
    for (int k = 0; k < NSPUS; k++) s_dur_ms[k] = 500;

    AcquireSRWLockExclusive(&s_jc_spu_lock);
    s_jc_spu_busy = (1u << NSPUS) - 1u;           /* hold every SPU number */
    ReleaseSRWLockExclusive(&s_jc_spu_lock);
    start_chain(JC2, CHAIN2, NSPUS);
    nap_ms(100);                                  /* every job waits for an SPU */
    for (int n = 0; n < NSPUS; n++) {
        jc_release_spu((u32)n);
        nap_ms(20);
    }
    finish_chain(JC2, NSPUS);
    if (s_total != NSPUS) FAIL("claim order: %ld of %d jobs ran", s_total, NSPUS);
    for (int k = 0; k < NSPUS; k++)
        if (s_spu[k] != k) {
            char got[64] = "";
            for (int j = 0; j < NSPUS; j++) {
                const size_t l = strlen(got);
                snprintf(got + l, sizeof got - l, " %d", s_spu[j]);
            }
            FAIL("jobs 0-%d ran on SPUs%s: they did not claim in chain order",
                 NSPUS - 1, got);
        }
}

/* A chain whose priority table allows SPUs 1-3 only (the table Red Dead
 * Redemption gives its Edge chain, see jc_set_priority) and whose
 * maxContention is 4: its jobs may still overlap, but each must run as one of
 * SPUs 1-3, so no more than three at once. */
enum { PRIO_JOBS = 6 };
static void check_priority_pool(void)
{
    static const u8 edge[8] = { 0, 6, 6, 6, 0, 0, 0, 0 };
    memcpy(vm_base + PRIO, edge, sizeof edge);
    u32 pc = CHAIN3;
    for (int k = 0; k < PRIO_JOBS; k++, pc += 8) put64(pc, JOBS + (u64)k * 0x100u);
    put64(pc, 7u | (15u << 3));                                               /* END */
    for (int k = 0; k < PRIO_JOBS; k++) s_dur_ms[k] = 60;

    s_running = s_max_running = s_total = 0;
    for (int k = 0; k < MAXJ; k++) s_spu[k] = -1;
    assert(cellSpursCreateJobChain(0, JC3, CHAIN3, 0x100, 4, PRIO, MAX_CONT, 0) == CELL_OK);
    assert(cellSpursRunJobChain(JC3) == CELL_OK);
    finish_chain(JC3, PRIO_JOBS);
    if (s_total != PRIO_JOBS) FAIL("priority table: %ld of %d jobs ran", s_total, PRIO_JOBS);
    for (int k = 0; k < PRIO_JOBS; k++)
        if (s_spu[k] < 1 || s_spu[k] > 3)
            FAIL("job %d ran as SPU %d, the chain's priority table allows SPUs 1-3", k, s_spu[k]);
    if (s_max_running > 3) FAIL("priority table allows 3 SPUs, %ld jobs ran at once", s_max_running);
    if (s_max_running < 2) FAIL("priority table chain never overlapped two jobs");
}

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
    s_spurs_event_queue[0] = 1;                   /* attach a queue to record completions */
    s_spurs_event_queue_n = 1;
    build_jobs();

    for (int k = 0; k < MAXJ; k++) s_dur_ms[k] = 40;
    s_dur_ms[0] = 160;                            /* job 0 finishes after jobs 1-3 */
    run_chain(JC, CHAIN, MAX_CONT);
    const long most = s_max_running;
    if (s_total != NJOBS) FAIL("%ld of %d jobs ran", s_total, NJOBS);
    check_done_order(JC, NJOBS);
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

        check_claim_order();
        check_done_order(JC2, NSPUS);

        check_priority_pool();
        check_done_order(JC3, PRIO_JOBS);
    }

    check_guard();
    printf("PASS %s (most jobs of the chain at once: %ld)\n", parallel ? "parallel" : "serial", most);
    return 0;
}
