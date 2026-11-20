/* A job chain carries on from the command it stopped at, and a Run is never lost.
 *
 * The guest pattern is an append-style chain (PlayStation All-Stars Battle
 * Royale queues its render jobs this way): each new command list is padded
 * with END, linked in by overwriting the END the chain last stopped on with a
 * NEXT to it, and the chain is Run again. So a walk has to resume AT that END
 * and re-read it. Starting over at the entry instead re-runs every job ever
 * queued, a walk longer each time. And a Run that lands while a walk is
 * finishing must still be walked: otherwise the walker returns on the END it
 * has already read, the new list never runs, and the title waits for those
 * jobs' results forever.
 *
 * The real libs/spurs/cellSpurs.c walker runs on its own thread. Only the job
 * binary dispatch (to count which jobs ran) and the completion-event push (to
 * hold the walker still at a chosen point) are observed; nothing in the walker
 * is replaced. */
#define spu_workload_dispatch_job fixture_dispatch_job
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_job
#include <assert.h>
#include <stdatomic.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x40000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }

enum { SPURS = 0x0800, PORT = 0x0900, CHAIN = 0x1000, ROOT = 0x2000,
       LISTS = 0x3000, JOBS = 0x10000, BIN = 0x20000, QUEUE = 3 };
#define LIST(k)   (LISTS + (u32)(k) * 0x100u)      /* 16 commands each */
#define JOBEA(k)  (JOBS + (u32)(k) * 0x80u)
#define END       0x7Full                          /* op 7, ext 15 */
#define NEXT(ea)  ((u64)(ea) | 3u)

/* How many times each job descriptor was dispatched. */
static atomic_uint runs[32];
int fixture_dispatch_job(const uint8_t* image, uint32_t size, uint32_t job_ea, uint32_t desc)
{
    (void)image; (void)size; (void)desc;
    assert(job_ea >= JOBS && (job_ea - JOBS) / 0x80u < 32);
    atomic_fetch_add(&runs[(job_ea - JOBS) / 0x80u], 1);
    return 1;
}

/* Completion events. The walker pushes one per job (data2 = the job's index in
 * the walk), then one at END repeating the last index. hold_next() blocks the
 * walker in its next push; hold_end() blocks it in the END push, i.e. after it
 * has read the END it stops on. The test acts while the walker sits there. */
static atomic_uint pushes, held, release_hold;
static atomic_int hold_mode;              /* 0 off, 1 next push, 2 END push */
static uint64_t hold_prev;                /* data2 of the last push since arming */
int sys_event_queue_push_by_id(uint32_t q, uint64_t src, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)q; (void)src; (void)d1; (void)d3;
    atomic_fetch_add(&pushes, 1);
    int mode = atomic_load(&hold_mode), stop = mode == 1;
    if (mode == 2) { stop = d2 == hold_prev; hold_prev = d2; }
    if (stop) {
        atomic_store(&hold_mode, 0);
        atomic_store(&held, 1);
        while (!atomic_load(&release_hold)) Sleep(1);
    }
    return 0;
}

static void job(int k)                 /* a plausible 16-byte binary */
{
    vm_write64(JOBEA(k) + 0x00, BIN);
    vm_write32(JOBEA(k) + 0x08, 1u << 16);
}
/* List k: these jobs, then END in every remaining slot, as the title pads it. */
static void list(int k, int n, const int* jobs)
{
    for (int i = 0; i < 16; i++) {
        if (i < n) job(jobs[i]);
        vm_write64(LIST(k) + 8u * i, i < n ? (u64)JOBEA(jobs[i]) : END);
    }
}
#define LIST_OF(k, ...) do { const int j_[] = { __VA_ARGS__ }; \
    list(k, (int)(sizeof j_ / sizeof j_[0]), j_); } while (0)

static int chain_slot(void)
{
    for (int i = 0; i < MAX_JOBCHAINS; i++) if (s_jobchains[i].jc_ea == CHAIN) return i;
    fprintf(stderr, "FAIL: the chain was not registered\n"); exit(1);
}
/* Wait until no walker is running and the events have stopped. */
static void quiet(void)
{
    int s = chain_slot(), still = 0;
    unsigned last = atomic_load(&pushes);
    for (int i = 0; i < 5000 && still < 10; i++) {
        Sleep(1);
        unsigned now = atomic_load(&pushes);
        still = (!s_jobchains[s].running && now == last) ? still + 1 : 0;
        last = now;
    }
    if (still < 10) { fprintf(stderr, "FAIL: the walker never stopped\n"); exit(1); }
}
static void arm(int mode)
{
    atomic_store(&held, 0); atomic_store(&release_hold, 0);
    hold_prev = ~0ull;
    atomic_store(&hold_mode, mode);
}
#define hold_next() arm(1)
#define hold_end()  arm(2)
static void wait_held(void)
{
    for (int i = 0; i < 5000 && !atomic_load(&held); i++) Sleep(1);
    if (!atomic_load(&held)) { fprintf(stderr, "FAIL: the walker never reached the hold\n"); exit(1); }
}
static void unhold(void) { atomic_store(&hold_mode, 0); atomic_store(&release_hold, 1); }

static int failed;
static void expect(int k, unsigned want, const char* what)
{
    unsigned got = atomic_load(&runs[k]);
    if (got != want) {
        fprintf(stderr, "FAIL: %s: job %d ran %u time(s), want %u\n", what, k, got, want);
        failed = 1;
    }
}

int main(void)
{
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)(uintptr_t)SPURS, QUEUE, (u8*)(uintptr_t)PORT, 1) == CELL_OK);
    for (int i = 0; i < 16; i++) vm_write64(ROOT + 8u * i, END);
    assert(cellSpursCreateJobChain(SPURS, CHAIN, ROOT, 256, 16, 0, 1, 0) == CELL_OK);

    /* The first list, linked from the root. */
    LIST_OF(1, 0, 1);
    vm_write64(ROOT, NEXT(LIST(1)));
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    quiet();
    expect(0, 1, "first list"); expect(1, 1, "first list");

    /* Append: the END after job 1 becomes a NEXT, then Run. Only the new jobs run. */
    LIST_OF(2, 2, 3);
    vm_write64(LIST(1) + 2 * 8, NEXT(LIST(2)));
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    quiet();
    expect(0, 1, "appended list (the walk must resume, not start over)");
    expect(1, 1, "appended list (the walk must resume, not start over)");
    expect(2, 1, "appended list"); expect(3, 1, "appended list");

    /* A Run that lands while the walk is finishing. Hold the walker in its END
     * event, i.e. after it has read the END it stops on; append and Run then. */
    LIST_OF(3, 4);
    vm_write64(LIST(2) + 2 * 8, NEXT(LIST(3)));
    hold_end();
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    wait_held();
    LIST_OF(4, 5);
    vm_write64(LIST(3) + 1 * 8, NEXT(LIST(4)));
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    unhold();
    quiet();
    expect(4, 1, "list run while a walk finished");
    expect(5, 1, "Run that arrived while the walk was finishing (must not be dropped)");

    /* A Run while the walk has not reached the END yet: it follows the new NEXT itself. */
    LIST_OF(5, 6);
    vm_write64(LIST(4) + 1 * 8, NEXT(LIST(5)));
    hold_next();                              /* job 6 done */
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    wait_held();
    LIST_OF(6, 7);
    vm_write64(LIST(5) + 1 * 8, NEXT(LIST(6)));
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    unhold();
    quiet();
    expect(6, 1, "list run during a walk"); expect(7, 1, "list appended during a walk");
    for (int k = 0; k <= 5; k++) expect(k, 1, "earlier jobs after more appends");

    /* Created again (The Simpsons Arcade Game creates its chain every frame):
     * the walk starts at the new entry. */
    LIST_OF(7, 8);
    assert(cellSpursCreateJobChain(SPURS, CHAIN, LIST(7), 256, 16, 0, 1, 0) == CELL_OK);
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    quiet();
    expect(8, 1, "re-created chain");

    /* Run again with nothing new where it stopped: the list runs again from its
     * entry, as it always has here (a title re-running a fixed list relies on it). */
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    quiet();
    expect(8, 2, "fixed list run again");

    /* After a Join, a Run starts over at the entry too. */
    assert(cellSpursJoinJobChain(CHAIN) == CELL_OK);
    LIST_OF(8, 9);
    vm_write64(LIST(7) + 1 * 8, NEXT(LIST(8)));
    assert(cellSpursRunJobChain(CHAIN) == CELL_OK);
    quiet();
    expect(8, 3, "Run after Join"); expect(9, 1, "Run after Join");

    if (failed) return 1;
    free(vm_base);
    puts("PASS: job chain resumes at the END it stopped on; no Run lost while a walk finishes; "
         "re-create, re-run of a fixed list and Join start at the entry");
    return 0;
}
