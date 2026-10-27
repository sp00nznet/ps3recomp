#include "spu_interp.h"
#include "platform/win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
u128 spu_rdch(spu_context*c,uint32_t ch){fprintf(stderr,"UNEXPECTED_CHANNEL pc=%x ch=%u\n",c->pc,ch);abort();}
void spu_wrch(spu_context*c,uint32_t ch,u128 v){(void)v;fprintf(stderr,"UNEXPECTED_CHANNEL pc=%x ch=%u\n",c->pc,ch);abort();}
uint32_t spu_rchcnt(spu_context*c,uint32_t ch){(void)c;(void)ch;abort();}
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context*c,uint32_t a){(void)c;(void)a;return NULL;}
void spu_spurs_taskset_syscall(spu_context*c){(void)c;abort();}
void spu_ls_watch_slow(uint32_t l,int w,const uint8_t*p,uint32_t pc,uint32_t lr){(void)l;(void)w;(void)p;(void)pc;(void)lr;abort();}
DWORD GetFileAttributesA(const char*p){(void)p;abort();}
DWORD GetLastError(void){return 0;}
void SetLastError(DWORD e){(void)e;}

/* Zero times an SPU extended-range finite number is zero, even though the
 * same bits encode an IEEE NaN/Infinity. FNMS then preserves its addend.
 * IBM SPU ISA v1.2 sections 9.1 and FNMS (pages 195/196/210).
 * This deliberately does not claim general extended multiplication support. */
#include "fnms_test_lifted.h"
static spu_context ctx;
static unsigned failures, checks;
static void check(const char *path, unsigned got, unsigned want,
                  unsigned a, unsigned b, unsigned c, unsigned lane) {
    ++checks;
    if(got!=want) {
        if(failures<12) fprintf(stderr,"FAIL %s lane%u a=%08x b=%08x c=%08x got=%08x want=%08x\n",path,lane,a,b,c,got,want);
        ++failures;
    }
}
static void put32(unsigned ea,unsigned v) {
    ctx.ls[ea]=v>>24;ctx.ls[ea+1]=v>>16;ctx.ls[ea+2]=v>>8;ctx.ls[ea+3]=v;
}
static void run_case(u128 a,u128 b,u128 c,u128 want) {
    u128 direct=spu_fnms(a,b,c);
    for(int i=0;i<4;i++) check("helper",direct._u32[i],want._u32[i],a._u32[i],b._u32[i],c._u32[i],i);
    const unsigned dests[]={12,3,5,7};
    for(unsigned k=0;k<4;k++) {
        unsigned d=dests[k];
        /* Synthetic RRR FNMS: destination 12 or alias of each input. */
        unsigned op=(13u<<28)|(d<<21)|(5u<<14)|(3u<<7)|7u;
        spu_context_init(&ctx,0);ctx.image_id=0;put32(0x100,op);
        ctx.gpr[3]=a;ctx.gpr[5]=b;ctx.gpr[7]=c;
        spu_interp_run_until(&ctx,0x100,0x104);
        if(ctx.pc!=0x104) abort();
        for(int i=0;i<4;i++) check("interpreter",ctx.gpr[d]._u32[i],want._u32[i],a._u32[i],b._u32[i],c._u32[i],i);
        ctx.gpr[3]=a;ctx.gpr[5]=b;ctx.gpr[7]=c;
        switch(k) {
        case 0: alias0_spu_func_00000100(&ctx);break;
        case 1: alias1_spu_func_00000100(&ctx);break;
        case 2: alias2_spu_func_00000100(&ctx);break;
        case 3: alias3_spu_func_00000100(&ctx);break;
        }
        for(int i=0;i<4;i++) check("lifted",ctx.gpr[d]._u32[i],want._u32[i],a._u32[i],b._u32[i],c._u32[i],i);
    }
}
int main(void) {
    const unsigned ext[]={0x7f800000,0x7f800001,0x7fc00000,0x7fffffff,0xff800000,0xff800001,0xffc00000,0xffffffff};
    /* Literal zero-product results, independently derived from the ISA. */
    const struct { unsigned bits, want; } add[]={
        {0,0},{0x80000000,0},{1,0},{0x807fffff,0},
        {0x00800000,0x00800000},{0x80800000,0x80800000},
        {0x3f800000,0x3f800000},{0xbf800000,0xbf800000},
        {0x7f7fffff,0x7f7fffff},{0xff7fffff,0xff7fffff},
        {0x7f800000,0x7f800000},{0xff800000,0xff800000},
        {0x7fffffff,0x7fffffff},{0xffffffff,0xffffffff}
    };
    for(unsigned order=0;order<2;order++) for(unsigned sign=0;sign<2;sign++)
    for(unsigned e=0;e<8;e++) for(unsigned c=0;c<14;c++) {
        u128 a,b,z,w;
        for(unsigned lane=0;lane<4;lane++) {
            unsigned v=ext[(e+lane)%8],q=add[(c+lane)%14].bits,zero=((sign+lane)%2)<<31;
            a._u32[lane]=order?v:zero;b._u32[lane]=order?zero:v;z._u32[lane]=q;
            w._u32[lane]=add[(c+lane)%14].want;
        }
        run_case(a,b,z,w);
    }
    /* Ordinary exact finite results, plus distinct lanes and signs. */
    u128 a={0},b={0},c={0},w={0};
    const float finite_want[]={3,7,9,9};
    for(int i=0;i<4;i++) {
        a._f32[i]=i+1;b._f32[i]=i-2;c._f32[i]=i*4+1;
        w._f32[i]=finite_want[i];
    }
    run_case(a,b,c,w);
    printf("FNMS zero/extended: 448 mixed-lane input cases, 1 ordinary finite control, 9 paths, 4 lanes; %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
