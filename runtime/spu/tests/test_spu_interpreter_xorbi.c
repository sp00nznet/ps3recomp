/* Real XORBI encodings must XOR the low immediate byte into all 16 bytes.
 * Literal vectors catch decoder omission, word/halfword immediate substitution,
 * byte-order/replication mistakes and source clobbering on destination alias. */
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
    p[0]=value>>24; p[1]=value>>16; p[2]=value>>8; p[3]=value;
}
static spu_context ctx;
static void run(uint32_t opcode,const uint32_t input[4],const uint32_t want[4],int alias) {
    unsigned ra=(opcode>>7)&127,rt=alias?ra:opcode&127;
    memset(&ctx,0,sizeof(ctx));spu_context_init(&ctx,0);ctx.image_id=-1;
    memcpy(ctx.gpr[ra]._u32,input,16);memset(&ctx.gpr[100],0x5A,16);
    be32(ctx.ls+0x100,(opcode&~127u)|rt);be32(ctx.ls+0x104,0);
    spu_interp_run(&ctx,0x100);
    if(ctx.status!=SPU_STATUS_STOPPED_BY_STOP || ctx.pc!=0x108) {
        fprintf(stderr,"XORBI %08X alias=%d stopped at %X status=%d\n",opcode,alias,ctx.pc,ctx.status);abort();
    }
    if(memcmp(ctx.gpr[rt]._u32,want,16)) {
        fprintf(stderr,"XORBI %08X alias=%d got %08X:%08X:%08X:%08X expected %08X:%08X:%08X:%08X\n",
                opcode,alias,ctx.gpr[rt]._u32[0],ctx.gpr[rt]._u32[1],ctx.gpr[rt]._u32[2],ctx.gpr[rt]._u32[3],
                want[0],want[1],want[2],want[3]);abort();
    }
    if(!alias)assert(!memcmp(ctx.gpr[ra]._u32,input,16));
    for(unsigned i=0;i<16;i++)assert(ctx.gpr[100]._u8[i]==0x5A);
#ifdef XORBI_LIFTED
    /* The same independent expectations also verify the real decoder/lifter
     * path, not only interpreter decode and existing helper dispatch. */
    memcpy(ctx.gpr[ra]._u32,input,16);
    if(rt!=ra)memset(&ctx.gpr[rt],0xA5,16);
    lifted_run((opcode&~127u)|rt,&ctx);
    assert(!memcmp(ctx.gpr[rt]._u32,want,16));
    if(!alias)assert(!memcmp(ctx.gpr[ra]._u32,input,16));
    for(unsigned i=0;i<16;i++)assert(ctx.gpr[100]._u8[i]==0x5A);
#endif
}
int main(void) {
    const uint32_t input[4]={0x00807FFF,0x12345678,0x89ABCDEF,0xFEDCBA98};
    static const struct {int imm;uint32_t want[4];} cases[]={
        {-512,{0x00807FFF,0x12345678,0x89ABCDEF,0xFEDCBA98}},
        {-257,{0xFF7F8000,0xEDCBA987,0x76543210,0x01234567}},
        {-256,{0x00807FFF,0x12345678,0x89ABCDEF,0xFEDCBA98}},
        {-255,{0x01817EFE,0x13355779,0x88AACCEE,0xFFDDBB99}},
        {-128,{0x8000FF7F,0x92B4D6F8,0x092B4D6F,0x7E5C3A18}},
        {-1,{0xFF7F8000,0xEDCBA987,0x76543210,0x01234567}},
        {0,{0x00807FFF,0x12345678,0x89ABCDEF,0xFEDCBA98}},
        {1,{0x01817EFE,0x13355779,0x88AACCEE,0xFFDDBB99}},
        {16,{0x10906FEF,0x02244668,0x99BBDDFF,0xEECCAA88}},
        {127,{0x7FFF0080,0x6D4B2907,0xF6D4B290,0x81A3C5E7}},
        {128,{0x8000FF7F,0x92B4D6F8,0x092B4D6F,0x7E5C3A18}},
        {255,{0xFF7F8000,0xEDCBA987,0x76543210,0x01234567}},
        {256,{0x00807FFF,0x12345678,0x89ABCDEF,0xFEDCBA98}},
        {511,{0xFF7F8000,0xEDCBA987,0x76543210,0x01234567}},
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        uint32_t op=0x46000000u|(((uint32_t)cases[i].imm&1023u)<<14)|(8u<<7)|7u;
        run(op,input,cases[i].want,0);run(op,input,cases[i].want,1);
    }
    puts("PASS: 28 encoded XORBI vectors, byte replication/truncation, and alias");return 0;
}
