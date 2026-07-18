/* SPU bring-up rung 3 — cellSync LFQueue work channel (host / PPU side).
 *
 * Uses the REAL libs/sync/cellSync.c LFQueue as the PPU->SPU work channel, with
 * its data buffer in shared "main memory":
 *   PPU  : cellSyncLFQueueInitialize + cellSyncLFQueuePush(work)   (real cellSync)
 *   SPU  : DMA-GET the queued item, process (echo) -> result slot, raise event
 *   PPU  : receive completion event, read result, cellSyncLFQueuePop (drain)
 *
 * This is flOw's PPU<->SPU work-distribution mechanism (cellSyncLFQueue* + SPU
 * DMA + a completion event) in miniature. Build requires linking cellSync.c:
 *   gcc -I out_lfqueue -I <runtime/spu> -I <libs/sync> \
 *       out_lfqueue/spu_recomp.c test_lfqueue_main.c <libs/sync/cellSync.c> -o test_lfqueue.exe
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include "cellSync.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* event-queue model (same as rung 2 / runtime sys_event.c) */
typedef struct { uint64_t source, data1, data2, data3; } ev_t;
typedef struct { ev_t buf[16]; int head, tail, count, active; } ev_queue;
static int evq_push(ev_queue* q, uint64_t s, uint64_t d1) {
    if (!q->active || q->count == 16) return -1;
    q->buf[q->tail] = (ev_t){s, d1, 0, 0}; q->tail = (q->tail + 1) % 16; q->count++; return 0;
}
static int evq_receive(ev_queue* q, ev_t* o) {
    if (!q->active || q->count == 0) return -1;
    *o = q->buf[q->head]; q->head = (q->head + 1) % 16; q->count--; return 0;
}

/* shared "main memory": layout = [0x100 LFQueue buffer (4x16B)] [0x140 result] */
static uint8_t  g_mem[1024];
uint8_t*        vm_base = g_mem;
static mfc_engine g_mfc;
static ev_queue g_spu_evq;

#define WORK_EA    0x100
#define RESULT_EA  0x140
#define EV_SOURCE_SPU_GROUP 0x7ULL

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    if (channel == SPU_WrOutIntrMbox) { evq_push(&g_spu_evq, EV_SOURCE_SPU_GROUP, value._u32[0]); return; }
    mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n"); }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}

/* diagnostics externs referenced from spu_dma.h in the full runtime */
int g_cri_video_dma = 0;
/* stop/halt hooks required by current lifter output */
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    spu_context ctx; spu_context_init(&ctx, 0);
    mfc_engine_init(&g_mfc);
    memset(&g_spu_evq, 0, sizeof(g_spu_evq)); g_spu_evq.active = 1;

    /* PPU: a real cellSync LFQueue whose data buffer lives in shared memory at
     * WORK_EA, so the SPU can DMA the queued items. */
    CellSyncLFQueue q;
    cellSyncLFQueueInitialize(&q, &g_mem[WORK_EA], /*elemSize*/16, /*depth*/4,
                              /*direction*/0, /*eaSignal*/0);

    /* PPU enqueues a work item (16 bytes, BE word0 = 0xC0DE0042). */
    uint8_t work[16] = {0};
    work[0]=0xC0; work[1]=0xDE; work[2]=0x00; work[3]=0x42;
    const uint32_t kWork = 0xC0DE0042u;
    int pushed = (cellSyncLFQueuePush(&q, work) == CELL_OK);

    /* SPU consumes the queued item (DMA), echoes it to RESULT_EA, raises event. */
    spu_func_00000000(&ctx);

    /* PPU: receive completion, read result, drain the queue. */
    ev_t evt; int got_evt = (evq_receive(&g_spu_evq, &evt) == 0);
    uint32_t result = be32(&g_mem[RESULT_EA]);
    uint8_t popped[16] = {0};
    int popped_ok = (cellSyncLFQueuePop(&q, popped) == CELL_OK);
    uint32_t pop_val = be32(popped);

    int push_ok   = pushed;
    int spu_ok    = (result == kWork);                    /* SPU read the queued item + produced output */
    int event_ok  = (got_evt && evt.data1 == 0x0D0E);     /* completion delivered                        */
    int pop_ok    = (popped_ok && pop_val == kWork);      /* real cellSync pop drains the queue          */

    printf("  [LFQ PUSH] PPU enqueued work 0x%08X                         %s\n", kWork, push_ok?"OK":"FAIL");
    printf("  [SPU DMA ] SPU -> result[0x140] = 0x%08X (expected 0x%08X)  %s\n", result, kWork, spu_ok?"OK":"FAIL");
    printf("  [EVENT   ] PPU got completion data1=0x%llX                   %s\n", (unsigned long long)evt.data1, event_ok?"OK":"FAIL");
    printf("  [LFQ POP ] PPU dequeued 0x%08X                              %s\n", pop_val, pop_ok?"OK":"FAIL");

    if (push_ok && spu_ok && event_ok && pop_ok)
        printf("  PASS: work flowed PPU -> cellSync LFQueue -> SPU (DMA) -> result + event -> PPU.\n");
    return (push_ok && spu_ok && event_ok && pop_ok) ? 0 : 1;
}
