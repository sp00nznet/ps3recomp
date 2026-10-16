#include "rsx_ao_position_dataflow.h"
#include <math.h>
static float current_constants[512][4];
/* Run the original recognizer's hostile encoding cases through the new path. */
#define rsx_ao_position_program_rows(p,n,r) rsx_ao_position_dataflow_rows(p,n,current_constants,r)
#define main original_cases
#include "test_ao_position_program.c"
#undef main
#undef rsx_ao_position_program_rows

static unsigned src(unsigned reg,unsigned component) {
    return 1u|(reg<<2)|(component*0x5500u);
}
static void third(program *p,unsigned value) {
    unsigned char *b=p->p+(p->n-1)*16;
    word(b+8,rsx_ao_pp_u32(b+8)|(value>>11));
    word(b+12,rsx_ao_pp_u32(b+12)|(value<<21));
}
static program forwarded(unsigned base,unsigned neutral,unsigned point) {
    program p={{0},0};
    emit(&p,1,0x1b02,0,0,3,15,point,31,1);
    emit(&p,7,0x1b01|(point<<2),0x1b03,base,0,1,10,31,1);
    emit(&p,7,0x1b01|(point<<2),0x1b03,base+1,0,8,1,31,1);
    emit(&p,7,0x1b01|(point<<2),0x1b03,base+2,0,4,63,0,1);
    emit(&p,7,0x1b01|(point<<2),0x1b03,base+3,0,4,10,31,1);
    emit(&p,1,0x1b02,0,0,4,15,point,31,1); /* old point dies, tokens survive */
    emit(&p,1,src(10,2),0,0,0,8,63,0,1);
    emit(&p,1,src(1,3),0,0,0,2,63,0,1);
    emit(&p,2,0x1b02,0xaa03,neutral,5,2,0,31,1);
    emit(&p,4,src(0,1),0x1b02,0,5,1,63,0,1);third(&p,src(10,0));
    return p;
}
static int dynamic_accepts(program p) {
    uint16_t out[4]={999,998,997,996};
    if(p.n)mutate(&p,p.n-1,3,1);
    int ok=rsx_ao_position_dataflow_rows(p.p,p.n*16,current_constants,out);
    if(!ok)CHECK(out[0]==999 && out[1]==998 && out[2]==997 && out[3]==996);
    return ok;
}
int main(void) {
    original_cases();
    for(unsigned r=0;r<512;++r)for(unsigned c=0;c<4;++c)current_constants[r][c]=1;
    for(unsigned base=0;base<508;base+=29)for(unsigned k=0;k<512;k+=37) {
        current_constants[k][2]=0;
        program p=forwarded(base,k,4);
        CHECK(dynamic_accepts(p));
        CHECK(!rsx_ao_position_program_rows(p.p,p.n*16,(uint16_t[4]){0}));
        current_constants[k][2]=-0.f;CHECK(dynamic_accepts(p));
        const float invalid[]={1,-1,NAN,INFINITY,-INFINITY};
        for(unsigned i=0;i<5;++i) {current_constants[k][2]=invalid[i];CHECK(!dynamic_accepts(p));}
        current_constants[k][2]=0;
        program bad=p;mutate(&bad,8,3,2);CHECK(!dynamic_accepts(bad)); /* indirect zero */
        bad=p;mutate(&bad,8,0,1u<<13);CHECK(!dynamic_accepts(bad)); /* conditional zero */
        bad=p;mutate(&bad,9,0,1u<<26);CHECK(!dynamic_accepts(bad)); /* saturates position */
        bad=p;mutate(&bad,6,1,1u);CHECK(!dynamic_accepts(bad)); /* forwarded lane changes */
        bad=p;mutate(&bad,1,0,1u<<26);CHECK(!dynamic_accepts(bad)); /* saturates saved projection */
        bad=p;mutate(&bad,2,1,1u<<12);CHECK(!dynamic_accepts(bad)); /* wrong row */
        bad=p;mutate(&bad,6,1,1u<<7);CHECK(!dynamic_accepts(bad)); /* negated token */
        current_constants[k][2]=1;
    }
    current_constants[400][2]=0;
    program p=forwarded(20,400,4),bad=p;
    /* Destroy one saved token, never allow the same temp index to restore it. */
    unsigned char *b=bad.p+5*16;
    word(b,(10u<<15)|(1u<<30));CHECK(!dynamic_accepts(bad));
    bad=p; /* Point changes between DP4 evaluations. */
    b=bad.p+1*16;word(b,rsx_ao_pp_u32(b)^(10u<<15)^(4u<<15));
    CHECK(!dynamic_accepts(bad));
    /* Neutral ADD uses src0 and src2, not src1. */
    bad=p;b=bad.p+9*16;word(b+4,(3u<<22)|(src(0,1)>>9));
    word(b+8,((src(0,1)&511u)<<23)|(src(10,0)>>11));
    CHECK(dynamic_accepts(bad));
    bad=p;mutate(&bad,9,1,8u<<27);CHECK(!dynamic_accepts(bad));
    /* Scalar temp63 is the no-write sentinel, not initialization of r63. */
    memset(&bad,0,sizeof(bad));
    emit(&bad,0,0x1b02,0,0,0,0,63,31,1);
    mutate(&bad,0,1,1u<<27);mutate(&bad,0,3,maskbits(15,20));
    rows(&bad,0,7,0x1b01|(63u<<2),0,0);CHECK(!dynamic_accepts(bad));
    uint16_t out[4]={777};
    CHECK(!rsx_ao_position_dataflow_rows(p.p,p.n*16,NULL,out));CHECK(out[0]==777);
    printf("AO dataflow: %u combined checks passed\n",checks);
    return 0;
}
