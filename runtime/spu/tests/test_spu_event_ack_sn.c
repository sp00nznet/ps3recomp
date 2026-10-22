/* SPU_WrEventAck through the real spu_channels.c: acknowledging Sn (0x2) while
 * a DMA list is still parked on a stall-and-notify (list_stall_stat non-zero)
 * leaves Sn pending. A resumed list runs inline, inside the MFC_WrListStallAck
 * that resumes it, so its next stall can land before the handler acknowledges
 * the event it was taken for; on hardware the MFC gets there after the ack.
 * Other bits, and Sn with no list parked, are acknowledged as before. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
extern void spu_wrch(spu_context*, uint32_t, u128);

static spu_context ctx;
static int fails;
static void ack(uint32_t bits) { spu_wrch(&ctx, SPU_WrEventAck, spu_splat_u32(bits)); }
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s (status 0x%X)\n", \
                                                     __LINE__, #c, ctx.event_status); } } while (0)

int main(void)
{
    static uint8_t ls[SPU_LS_SIZE];
    spu_context_init(&ctx, 0);
    ctx.ls = ls;

    /* A list parked on tag 4: Sn stays pending through its ack, Lr is acked. */
    ctx.list_stall_mask = ctx.list_stall_stat = 1u << 4;
    ctx.event_status = 0x2u | 0x400u;
    ack(0x2u | 0x400u);
    CHECK(ctx.event_status == 0x2u);

    /* Nothing parked: Sn is acked like any other event. */
    ctx.list_stall_mask = ctx.list_stall_stat = 0;
    ack(0x2u);
    CHECK(ctx.event_status == 0);

    /* An ack without Sn does not raise it, parked list or not. */
    ctx.list_stall_stat = 1u << 4;
    ctx.event_status = 0x1u;
    ack(0x1u);
    CHECK(ctx.event_status == 0);

    printf("%s: WrEventAck keeps Sn pending while a list is parked\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
void ppu_resv_break_store(uint64_t ea){(void)ea;abort();}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;abort();}
unsigned long long ps3_ms_now(void){abort();}
uint32_t g_ww_lo=0xFFFFFFFFu,g_ww_hi;int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
