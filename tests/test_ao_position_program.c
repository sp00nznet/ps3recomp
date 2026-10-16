#include "rsx_ao_position_program.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct { unsigned char p[512*16]; unsigned n; } program;
static void word(unsigned char *p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(i*8)); }
static void mutate(program *p,unsigned instruction,unsigned dword,uint32_t mask) {
    unsigned char *b=p->p+instruction*16+dword*4;word(b,rsx_ao_pp_u32(b)^mask);
}
static unsigned maskbits(unsigned mask,unsigned start) {
    unsigned result=0;for(unsigned c=0;c<4;++c)if(mask&(1u<<c))result|=1u<<(start-c);return result;
}
static void emit(program *p,unsigned op,unsigned s0,unsigned s1,unsigned row,
    unsigned input,unsigned mask,unsigned temp,unsigned output,int vec_result) {
    unsigned char *b=p->p+p->n++*16;CHECK(p->n<=512);
    word(b,(temp<<15)|((unsigned)vec_result<<30));
    word(b+4,(op<<22)|(row<<12)|(input<<8)|(s0>>9));
    word(b+8,((s0&511)<<23)|(s1<<6));
    word(b+12,(output<<2)|(63u<<7)|maskbits(mask,16));
}
static void rows(program *p,unsigned base,unsigned op,unsigned point,unsigned input,int swap) {
    for(unsigned j=0;j<4;++j) {
        unsigned c=3-j;emit(p,op,swap?0x1b03:point,swap?point:0x1b03,base+c,input,1u<<c,63,0,1);
    }
}
static int accepts(program p) {
    uint16_t out[4]={999,998,997,996};
    if(p.n)mutate(&p,p.n-1,3,1);
    int ok=rsx_ao_position_program_rows(p.p,p.n*16,out);
    if(!ok)CHECK(out[0]==999 && out[1]==998 && out[2]==997 && out[3]==996);
    return ok;
}
int main(void) {
    program p={{0},0};uint16_t out[4]={0};
    CHECK(!rsx_ao_position_program_rows(NULL,64,out));CHECK(!rsx_ao_position_program_rows(p.p,64,NULL));
    CHECK(!rsx_ao_position_program_rows(p.p,0,out));CHECK(!rsx_ao_position_program_rows(p.p,65,out));
    CHECK(!rsx_ao_position_program_rows(p.p,513*16,out));
    for(unsigned base=0;base<=508;base+=4)for(unsigned input=0;input<16;input+=3)for(int swap=0;swap<2;++swap) {
        memset(&p,0,sizeof(p));rows(&p,base,7,0x1b02,input,swap);CHECK(accepts(p));
    }
    memset(&p,0,sizeof(p));rows(&p,509,7,0x1b02,0,0);CHECK(!accepts(p));
    memset(&p,0,sizeof(p));rows(&p,256,7,0x1b02,0,0);
    program good=p;
    /* vecResult false means vector output is NOT position. */
    for(unsigned i=0;i<4;++i)mutate(&p,i,0,1u<<30);
    CHECK(!accepts(p));
    p=good;mutate(&p,0,3,1);CHECK(!accepts(p));
    CHECK(!rsx_ao_position_program_rows(good.p,64,out)); /* no END */
    const unsigned dw[]={0,0,0,2,3,1,1,1,3};
    const uint32_t flip[]={1u<<13,1u<<26,1u<<21,1u<<14,2u,1u<<22,1u<<8,1u<<12,1u<<14};
    for(unsigned i=0;i<sizeof(dw)/sizeof(dw[0]);++i) { p=good;mutate(&p,0,dw[i],flip[i]);CHECK(!accepts(p)); }
    p=good;mutate(&p,0,0,(1u<<13)|(7u<<10));CHECK(accepts(p));
    for(unsigned flow=8;flow<=31;++flow) {
        if(flow>=13 && flow<=16)continue;
        p=good;mutate(&p,0,1,flow<<27);CHECK(!accepts(p));
    }
    for(unsigned op=23;op<=31;++op)if(op!=25) { p=good;mutate(&p,0,1,(7u^op)<<22);CHECK(!accepts(p)); }
    for(unsigned reg=0;reg<63;reg+=7)for(unsigned op=6;op<=7;++op) {
        unsigned point=(op==6?0x1801:0x1b01)|(reg<<2);
        memset(&p,0,sizeof(p));emit(&p,1,0x1b02,0x1b03,0,0,op==6?7:15,reg,31,1);
        rows(&p,0,op,point,0,0);CHECK(accepts(p));
        program temp=p;
        p=temp;mutate(&p,0,0,1u<<13);CHECK(!accepts(p)); /* predicated initialization */
        p=temp;mutate(&p,0,1,(1u^13u)<<22);CHECK(!accepts(p)); /* ARL is not temp init */
        p=temp;mutate(&p,0,1,1u<<22);CHECK(!accepts(p)); /* NOP is not temp init */
        p=temp; /* unrelated output after position */
        emit(&p,1,0x1b02,0x1b03,0,0,15,63,1,1);CHECK(accepts(p));
        p=temp; /* source mutation after all position writes is harmless */
        emit(&p,1,0x1b02,0x1b03,0,0,15,reg,31,1);CHECK(accepts(p));
        for(unsigned component=0;component<4;++component) {
            memset(&p,0,sizeof(p));emit(&p,1,0x1b02,0x1b03,0,0,15,reg,31,1);
            emit(&p,op,point,0x1b03,3,0,8,63,0,1);
            emit(&p,1,0x1b02,0x1b03,0,0,1u<<component,reg,31,1);
            for(unsigned c=0;c<3;++c)emit(&p,op,point,0x1b03,c,0,1u<<c,63,0,1);
            CHECK(accepts(p)==(op==6 && component==3));
            /* Same mutation through scalar MOV, temp vector NOP. */
            unsigned char *b=p.p+2*16;
            word(b+4,rsx_ao_pp_u32(b+4)^(1u<<22)^(1u<<27));
            word(b+12,(31u<<2)|(reg<<7)|maskbits(1u<<component,20));
            CHECK(accepts(p)==(op==6 && component==3));
        }
        /* Stable point despite unrelated temp writes between rows. */
        memset(&p,0,sizeof(p));emit(&p,1,0x1b02,0x1b03,0,0,15,reg,31,1);
        for(unsigned c=0;c<4;++c) {
            emit(&p,op,point,0x1b03,c,0,1u<<c,63,0,1);
            emit(&p,1,0x1b02,0x1b03,0,0,15,(reg+1)%63,31,1);
        }CHECK(accepts(p));
    }
    /* Scalar position output must invalidate the candidate. */
    p=good;emit(&p,0,0x1b02,0x1b03,0,0,0,63,0,0);
    mutate(&p,4,1,1u<<27);mutate(&p,4,3,1u<<20);CHECK(!accepts(p));
    /* Scalar NOP with stale mask/output fields does not write position. */
    p=good;emit(&p,0,0x1b02,0x1b03,0,0,0,63,0,0);mutate(&p,4,3,1u<<20);CHECK(accepts(p));
    p=good;while(p.n<512)emit(&p,0,0,0,0,0,0,63,31,1);CHECK(accepts(p));
    printf("AO position program: %u checks passed\n",checks);return 0;
}
