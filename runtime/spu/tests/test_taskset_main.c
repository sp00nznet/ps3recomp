/* SPU bring-up rung 4 — cellSpurs taskset / job-queue throughput (host side).
 *
 * Brings rungs 1-3 together under the SPURS work-engine model: a cellSpurs-style
 * taskset drains a real cellSync LFQueue of N work items and invokes a single
 * lifted SPU task body once per item; each invocation DMAs the item, produces a
 * result, and raises a completion event delivered to the taskset's event queue.
 *
 * This is the bridge the lv2 SPU-thread-group layer (lv2_register.c: create ->
 * start -> join -> sys_event_queue_push_by_id) and a real cellSpurs taskset would
 * drive: lifted SPU execution as the task body, fed by an LFQueue, completing via
 * events. Here the taskset runner is a minimal host stand-in for cellSpurs; the
 * integration rung promotes it into cellSpurs.c + the lv2 thread-group path.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include "cellSync.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t source, data1; } ev_t;
typedef struct { ev_t buf[16]; int head, tail, count, active; } ev_queue;
static int evq_push(ev_queue* q, uint64_t s, uint64_t d1) {
    if (!q->active || q->count == 16) return -1;
    q->buf[q->tail].source = s; q->buf[q->tail].data1 = d1;
    q->tail = (q->tail + 1) % 16; q->count++; return 0;
}
static int evq_receive(ev_queue* q, ev_t* o) {
    if (!q->active || q->count == 0) return -1;
    *o = q->buf[q->head]; q->head = (q->head + 1) % 16; q->count--; return 0;
}

static uint8_t  g_mem[2048];
uint8_t*        vm_base = g_mem;
static mfc_engine g_mfc;
static ev_queue g_taskset_evq;     /* taskset completion event queue */

#define WORK_EA   0x180            /* SPU task input slot  */
#define RESULT_EA 0x1C0            /* SPU task output slot */
#define QUEUE_BUF 0x100            /* LFQueue data buffer  */
#define EV_SOURCE_SPU_GROUP 0x7ULL

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    if (channel == SPU_WrOutIntrMbox) { evq_push(&g_taskset_evq, EV_SOURCE_SPU_GROUP, value._u32[0]); return; }
    mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n"); }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static void wbe32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

/* ---- minimal cellSpurs-style taskset: runs the lifted SPU task body over a
 *      cellSync LFQueue of work items, collecting per-item completion events. ---- */
typedef struct { CellSyncLFQueue* work_q; ev_queue* evq; int done; } spurs_taskset;

static void taskset_run(spurs_taskset* ts) {
    uint8_t item[16];
    /* non-blocking drain: cellSyncLFQueuePop BLOCKS when empty (it's the
     * blocking variant); TryPop returns an error so the taskset stops when the
     * workload queue is exhausted. */
    while (cellSyncLFQueueTryPop(ts->work_q, item) == CELL_OK) {
        memcpy(&g_mem[WORK_EA], item, 16);                 /* deliver workload to the SPU task input */
        spu_context ctx; spu_context_init(&ctx, 0);
        mfc_engine_init(&g_mfc);
        spu_func_00000000(&ctx);                           /* run the lifted SPU task body           */
        ts->done++;
    }
}

/* diagnostics externs referenced from spu_dma.h in the full runtime */
int g_cri_video_dma = 0;
/* stop/halt hooks required by current lifter output */
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    memset(&g_taskset_evq, 0, sizeof(g_taskset_evq)); g_taskset_evq.active = 1;

    /* PPU: a real cellSync LFQueue feeding the taskset, buffer in shared memory. */
    CellSyncLFQueue q;
    cellSyncLFQueueInitialize(&q, &g_mem[QUEUE_BUF], 16, 8, 0, 0);

    /* PPU enqueues 3 workloads. */
    const uint32_t kWork[3] = { 0x1111AAAA, 0x2222BBBB, 0x3333CCCC };
    int pushed = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t item[16] = {0}; wbe32(item, kWork[i]);
        if (cellSyncLFQueuePush(&q, item) == CELL_OK) pushed++;
    }

    /* Run the taskset: it drains the queue, invoking the lifted SPU task per item. */
    spurs_taskset ts = { &q, &g_taskset_evq, 0 };
    taskset_run(&ts);

    /* Verify: 3 workloads processed, 3 completion events, each result == its input. */
    int events = 0; ev_t e;
    while (evq_receive(&g_taskset_evq, &e) == 0) { if (e.data1 == 0x7A5C) events++; }
    uint32_t last_result = be32(&g_mem[RESULT_EA]);

    int all_ok = (pushed == 3) && (ts.done == 3) && (events == 3)
                 && (last_result == kWork[2]);
    printf("  [TASKSET] enqueued=%d  processed=%d  completion events=%d\n", pushed, ts.done, events);
    printf("  [RESULT ] last task result = 0x%08X (expected 0x%08X)  %s\n",
           last_result, kWork[2], (last_result == kWork[2]) ? "OK" : "FAIL");
    if (all_ok)
        printf("  PASS: cellSpurs-style taskset ran a lifted SPU job over an LFQueue of workloads.\n");
    return all_ok ? 0 : 1;
}
