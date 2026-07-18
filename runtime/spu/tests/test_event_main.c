/* SPU bring-up rung 2 — event-driven completion (host / PPU side).
 *
 * Builds on the bring-up test: the SPU produces output in shared memory AND
 * raises completion via the interrupt mailbox (SPU_WrOutIntrMbox). The harness
 * models the connected event queue exactly as runtime/syscalls/sys_event.c does
 * (a sys_event_t {source,data1,data2,data3} ring buffer; the SPU interrupt is
 * delivered via the equivalent of sys_event_queue_push_by_id), and the PPU
 * "receives" the completion event (the equivalent of sys_event_queue_receive),
 * verifying source + payload.
 *
 * This is the second rung of the flOw SPU workstream: the SPU signals completion
 * through the *event-queue* path flOw actually uses (sys_spu_thread_group_connect_event
 * -> sys_event_queue_receive), not just a polled mailbox. The integrated path wires
 * the real sys_event.c (push_by_id / receive) to the lv2 SPU thread-group layer.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---- event-queue model (mirrors runtime/syscalls/sys_event.{h,c}) ---- */
typedef struct { uint64_t source, data1, data2, data3; } ev_t;
typedef struct { ev_t buf[16]; int head, tail, count, active; } ev_queue;

static int evq_push(ev_queue* q, uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3) {
    if (!q->active || q->count == 16) return -1;
    q->buf[q->tail] = (ev_t){source, d1, d2, d3};
    q->tail = (q->tail + 1) % 16; q->count++;
    return 0;
}
static int evq_receive(ev_queue* q, ev_t* out) {     /* blocking-style pop */
    if (!q->active || q->count == 0) return -1;
    *out = q->buf[q->head];
    q->head = (q->head + 1) % 16; q->count--;
    return 0;
}

/* source tag for a SYS_SPU_THREAD_GROUP completion event (matches the runtime's
 * convention of tagging the event source with the SPU group id). */
#define EV_SOURCE_SPU_GROUP  0x0000000000000007ULL

/* ---- "main memory" + the SPU's connected event queue ---- */
static uint8_t  g_mem[1024];
uint8_t*        vm_base = g_mem;
static mfc_engine g_mfc;
static ev_queue g_spu_evq;   /* event queue connected to the SPU (group id 7) */

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    if (channel == SPU_WrOutIntrMbox) {
        /* SPU raised a completion interrupt -> deliver as a thread-group event
         * to the connected queue (the runtime does this via
         * sys_event_queue_push_by_id from the SPU thread-group join path). */
        evq_push(&g_spu_evq, EV_SOURCE_SPU_GROUP, value._u32[0], 0, 0);
        return;
    }
    mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);   /* MFC -> DMA */
}
void spu_indirect_branch(spu_context* ctx) {
    (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n");
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* diagnostics externs referenced from spu_dma.h in the full runtime */
int g_cri_video_dma = 0;
/* stop/halt hooks required by current lifter output */
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));

    spu_context ctx;
    spu_context_init(&ctx, 0);
    mfc_engine_init(&g_mfc);

    /* PPU side: create + connect an event queue for this SPU (the runtime does
     * sys_event_queue_create + sys_spu_thread_group_connect_event here). */
    memset(&g_spu_evq, 0, sizeof(g_spu_evq));
    g_spu_evq.active = 1;

    /* Run the SPU job: produce a result via DMA PUT, raise completion interrupt. */
    spu_func_00000000(&ctx);

    /* PPU side: receive the completion event (sys_event_queue_receive). */
    ev_t evt;
    int got = (evq_receive(&g_spu_evq, &evt) == 0);

    const uint32_t kExpect = 0x00001234u;
    uint32_t put_val = be32(&g_mem[0]);
    int put_ok   = (put_val == kExpect);                                  /* SPU -> main memory   */
    int event_ok = (got && evt.source == EV_SOURCE_SPU_GROUP
                        && evt.data1 == kExpect);                         /* SPU -> event -> PPU  */

    printf("  [DMA PUT ] SPU -> main_mem[0] = 0x%08X (expected 0x%08X)  %s\n",
           put_val, kExpect, put_ok ? "OK" : "FAIL");
    printf("  [EVENT   ] PPU received completion: got=%d source=0x%llX data1=0x%llX  %s\n",
           got, (unsigned long long)evt.source, (unsigned long long)evt.data1,
           event_ok ? "OK" : "FAIL");

    if (put_ok && event_ok)
        printf("  PASS: SPU completion delivered through the event-queue path to the PPU.\n");
    return (put_ok && event_ok) ? 0 : 1;
}
