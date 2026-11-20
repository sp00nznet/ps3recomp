/* ILH must replicate the immediate in every 16-bit lane. Real interpreter
 * bytes plus literal full-vector expectations; no title assets are included. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPU-only instructions never reach these external channel/diagnostic hooks. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

static void be32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static spu_context ctx;
static void run(uint32_t opcode,const uint32_t input[4],const uint32_t expected[4],int alias) {
 memset(&ctx,0,sizeof(ctx));spu_context_init(&ctx,0);ctx.image_id=-1;
 memcpy(ctx.gpr[8]._u32,input,16);memset(&ctx.gpr[9],0x5A,16);
 be32(ctx.ls+0x100,opcode);be32(ctx.ls+0x104,0);
 spu_interp_run(&ctx,0x100);
 assert(ctx.status==SPU_STATUS_STOPPED_BY_STOP && ctx.pc==0x108);
 assert(!memcmp(ctx.gpr[alias?8:7]._u32,expected,16));
 if(!alias)assert(!memcmp(ctx.gpr[8]._u32,input,16));
 for(unsigned i=0;i<16;i++)assert(ctx.gpr[9]._u8[i]==0x5A);
}
int main(void) {
 const uint32_t input[4]={0xABCD0001,0x12347FFF,0xCDEF8000,0x7654FFFF};
 static const struct { int imm;uint32_t expected[4]; } cases[]={
 {0,{0x00000000u,0x00000000u,0x00000000u,0x00000000u}},
 {1,{0x00000001u,0x00007FFFu,0x00008000u,0x0000FFFFu}},
 {511,{0x000001FFu,0x00FF7E01u,0x00FF8000u,0x01FEFE01u}},
 {-1,{0x0000FFFFu,0x7FFE8001u,0x7FFF8000u,0xFFFE0001u}},
 {-512,{0x0000FE00u,0x7EFF0200u,0x7F000000u,0xFDFF0200u}},
 {224,{0x000000E0u,0x006FFF20u,0x00700000u,0x00DFFF20u}},
 };
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
  uint32_t opcode=0x75000000u|(((uint32_t)cases[i].imm&0x3FFu)<<14)|(8u<<7)|7u;
  run(opcode,input,cases[i].expected,0);
  run((opcode&~0x7Fu)|8u,input,cases[i].expected,1);
 } puts("PASS: unsigned immediate multiply, low halfword selection, signed immediate extension, all lanes and alias");
 return 0;
}
