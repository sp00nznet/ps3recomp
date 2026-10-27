/* Architecture regression: machine-status IE follows the interrupt latch,
 * independently of run/stop/channel state. Exercises the real channel, full
 * interpreter channel instruction, and SMC branch feature bits. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern u128 spu_rdch(spu_context*, uint32_t);
extern uint32_t spu_rchcnt(spu_context*, uint32_t);
static spu_context ctx;
static unsigned checks;
static void check(const char* path, unsigned want) {
    u128 got = spu_rdch(&ctx, SPU_RdMachStat);
    if (got._u32[0] != want) {
        fprintf(stderr, "%s: status=%X interrupt-enable=%u machine-status=%X expected=%X\n",
                path, ctx.status, ctx.int_enable, got._u32[0], want);
        exit(1);
    }
    assert(spu_rchcnt(&ctx, SPU_RdMachStat) == 1);
    checks++;
}
static void word(uint32_t pc, uint32_t opcode) {
    ctx.ls[pc]=opcode>>24;ctx.ls[pc+1]=opcode>>16;
    ctx.ls[pc+2]=opcode>>8;ctx.ls[pc+3]=opcode;
}
static void landed(spu_context* c) { (void)c; }
int main(void) {
    spu_context_init(&ctx, 0);
    const unsigned states[] = {SPU_STATUS_RUNNING, SPU_STATUS_STOPPED,
        SPU_STATUS_STOPPED_BY_STOP, SPU_STATUS_STOPPED_BY_HALT,
        SPU_STATUS_WAITING_CHANNEL};
    for (unsigned s=0;s<sizeof(states)/sizeof(states[0]);s++)
        for (unsigned ie=0;ie<2;ie++) {
            ctx.status=states[s];ctx.int_enable=ie;check("channel",ie);
        }
    for (unsigned ie=0;ie<2;ie++) {
        ctx.image_id=-1;ctx.int_enable=ie;
        word(0x4000,0x01A00683); /* rdch $3, SPU_RdMachStat */
        word(0x4004,0x00002000); /* stop 0x2000 */
        spu_interp_run(&ctx,0x4000);
        if(ctx.gpr[3]._u32[0]!=ie) {
            fprintf(stderr,"interpreter: IE=%u read=%X\n",ie,ctx.gpr[3]._u32[0]);return 1;
        }
        checks++;
    }
    spu_begin_image(77);spu_register_function(0x240,landed);
    for(unsigned ie=0;ie<2;ie++) {
        ctx.image_id=77;ctx.policy_mode=1;ctx.status=SPU_STATUS_RUNNING;
        ctx.int_enable=1-ie;ctx.gpr[7]=spu_splat_u32(0x240);ctx.pc=0x4000;
        word(0x4000,0x35000000|(7<<7)|(ie?0x40000:0x80000)); /* bi E/D $7 */
        spu_indirect_branch(&ctx);
        assert(ctx.int_enable==ie);check("branch feature bit",ie);
        g_spu_trampoline_fn=0;
    }
    printf("PASS: %u machine-status channel cases\n",checks);return 0;
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
/* LV2 worker publication is outside the machine-status paths exercised here. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
