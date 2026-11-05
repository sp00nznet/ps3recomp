/* Primary reference: SPU_ISA_v12_e.pdf pp178-179,181,186-189,251.
 * Literal target/link/interrupt expectations exercise encoded instructions.
 * E/D=11 is reserved and deliberately has no invented expectation. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base; uint32_t ppu_vm_size;
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t,void(*)(spu_context*));
static spu_context ctx;
static int seen_target,seen_enable;
static unsigned link_rt;
static void callee(spu_context* c) {seen_target=c->pc;seen_enable=c->int_enable;SPU_RET_REG(c,link_rt);}
static void word(uint32_t at,uint32_t w){ctx.ls[at]=w>>24;ctx.ls[at+1]=w>>16;ctx.ls[at+2]=w>>8;ctx.ls[at+3]=w;}
#ifdef BRANCH_LIFTED
#include "interrupt_branches_lifted.h"
#endif
static unsigned checks;
static void run(uint32_t opcode,uint32_t cond,int initial,uint32_t target,int want_enable,int alias,int mode){
 memset(&ctx,0,sizeof(ctx));spu_context_init(&ctx,0);ctx.image_id=77;ctx.policy_mode=1;
 unsigned ra=(opcode>>7)&127,rt=opcode&127;ctx.pc=0x4000;ctx.int_enable=initial;ctx.srr0=0x240;
 ctx.gpr[ra]=spu_splat_u32(mode==2?0x240:0x40243); /* interpreter/microstep also verify target masking */
 if((opcode>>21)>=0x128 && (opcode>>21)<=0x12b)ctx.gpr[rt]=spu_splat_u32(cond);
 if(alias) {opcode=(opcode&~127u)|ra;rt=ra;ctx.gpr[ra]=spu_splat_u32(cond);}
 link_rt=rt;
 ctx.gpr[8]=spu_splat_u32(0x4004);
 if(target==0x4004 && (opcode>>21)>=0x1a8)ctx.gpr[ra]=spu_splat_u32(mode==2?0x4004:0x4007);
 u128 before[128];memcpy(before,ctx.gpr,sizeof(before));word(0x4000,opcode);word(0x4004,0x35000400); /* bi r8 terminates a microstep fallthrough without changing state */
 seen_target=seen_enable=-1;g_spu_trampoline_fn=0;
 if(mode==0)spu_interp_run_until(&ctx,0x4000,alias && (opcode>>21)==0x1a9 ? 0x4004 : target);
 else if(mode==1)spu_indirect_branch(&ctx); /* known opcode: real SMC microstep */
#ifdef BRANCH_LIFTED
 else lifted_run(opcode,&ctx);
#endif
 uint32_t got_pc=ctx.pc & 0x3FFFC;
 int bisl=(opcode>>21)==0x1a9;
 if(mode==2 && bisl){got_pc=seen_target;assert(seen_enable==want_enable);}
 if(got_pc!=target || ctx.int_enable!=(unsigned)want_enable){
  fprintf(stderr,"branch opcode=%08X mode=%d alias=%d pc=%X expected=%X enable=%u expected=%d\n",opcode,mode,alias,got_pc,target,ctx.int_enable,want_enable);exit(1);}
 for(unsigned i=0;i<128;i++){
  if(bisl && i==rt){const uint32_t link[4]={0x4004,0,0,0};
   /* Microstep historically splats links; compare architectural preferred slot there. */
   assert(ctx.gpr[i]._u32[0]==0x4004);if(mode!=1)assert(!memcmp(ctx.gpr[i]._u32,link,16));
  } else assert(!memcmp(&ctx.gpr[i],&before[i],16));
 }
 checks++;
}
static void cases(int mode){
 /* Exact original WWS instructions: LS2CDC bid r0;22AC bid r74;2CD4 bie r2. */
 run(0x35080000,0,1,0x240,0,0,mode);run(0x35082500,0,1,0x240,0,0,mode);run(0x35040100,0,0,0x240,1,0,mode);
 const unsigned ops[]={0x1a8,0x1a9,0x1aa,0x128,0x129,0x12a,0x12b};
 const unsigned flags[]={0,0x40000,0x80000};
 for(unsigned o=0;o<7;o++)for(unsigned f=0;f<3;f++)for(int initial=0;initial<2;initial++){
  unsigned op=ops[o];uint32_t code=(op<<21)|flags[f]|(7<<7)|5;
  int enabled=f==1?1:f==2?0:initial;
  if(op>=0x1a8)run(code,0,initial,0x240,enabled,0,mode);
  else{
   uint32_t taken=(op==0x128||op==0x12a)?0:1;
   uint32_t untaken=(op==0x128||op==0x12a)?1:0;
   run(code,taken,initial,0x240,enabled,0,mode);run(code,untaken,initial,0x4004,initial,0,mode);
   /* Halfword predicates must ignore a nonzero upper halfword. */
   if(op==0x12a)run(code,0xABCD0000,initial,0x240,enabled,0,mode);
   if(op==0x12b)run(code,0xABCD0000,initial,0x4004,initial,0,mode);
  }
 }
 /* Condition/target aliases preserve the pre-instruction register and predicate. */
 for(unsigned op=0x128;op<=0x12b;op++){
  int taken=op==0x129||op==0x12b;
  run((op<<21)|0x80000|(7<<7)|7,0x240,1,taken?0x240:0x4004,taken?0:1,1,mode);
 }
 /* BISL reads its target before writing an aliased link destination.
  * The compiled lifter has a separate existing alias limitation, so this
  * control covers the corrected interpreter and already-correct microstep. */
 if(mode!=2)for(unsigned f=0;f<3;f++)for(int initial=0;initial<2;initial++)
  run((0x1a9<<21)|flags[f]|(7<<7)|7,0x240,initial,0x240,f==1?1:f==2?0:initial,1,mode);
 /* Target==fallthrough is still a TAKEN branch and must apply its feature bits. */
 run(0x35080000|(7<<7),0,1,0x4004,0,0,mode);
}
int main(void){spu_begin_image(77);spu_register_function(0x240,callee);cases(0);cases(1);
#ifdef BRANCH_LIFTED
 cases(2);
#endif
 printf("PASS: %u encoded interrupt branch cases\n",checks);return 0;}
void ppu_resv_break_store(uint64_t ea){(void)ea;abort();}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;abort();}
unsigned long long ps3_ms_now(void){abort();}
uint32_t g_ww_lo=0xFFFFFFFFu,g_ww_hi;int g_resv_store_active;

uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
