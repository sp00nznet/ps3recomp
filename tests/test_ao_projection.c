#include "../libs/video/rsx_ao_projection.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void word(unsigned char *p,uint32_t v) {
    for(unsigned i=0;i<4;++i) p[i]=(unsigned char)(v>>(8*i));
}
static void program(unsigned char p[64],unsigned base,unsigned swap) {
    for(unsigned i=0;i<4;++i) {
        unsigned c=3-i,s0=swap?0x1b03:0x1b02,s1=swap?0x1b02:0x1b03;
        word(p+16*i,(1u<<30)|(63u<<15));
        word(p+16*i+4,(7u<<22)|((base+c)<<12)|(s0>>9));
        word(p+16*i+8,((s0&511)<<23)|(s1<<6));
        word(p+16*i+12,((8u>>c)<<13)|(i==3));
    }
}
int main(void) {
    unsigned char p[64],bad[64]; uint16_t rows[4];
    for(unsigned swap=0;swap<2;++swap) for(unsigned base=0;base<=508;base+=4) {
        program(p,base,swap); CHECK(rsx_ao_position_rows(p,64,rows));
        for(unsigned j=0;j<4;++j) CHECK(rows[j]==base+j);
    }
    CHECK(!rsx_ao_position_rows(NULL,64,rows));
    CHECK(!rsx_ao_position_rows(p,63,rows));
    CHECK(!rsx_ao_position_rows(p,65,rows));
    CHECK(!rsx_ao_position_rows(p,64,NULL));
    program(p,256,0);
    memcpy(bad,p,64); word(bad,rsx_ao_word(bad)|(1u<<13)|(7u<<10));
    CHECK(rsx_ao_position_rows(bad,64,rows)); /* unconditional TRUE predicate */
    const unsigned word_index[]={0,0,0,0,1,1,1,2,3,3,3,3};
    const uint32_t flips[]={1u<<13,1u<<21,1u<<26,1u<<15,1u<<22,1u<<27,1u<<8,1u<<14,1u,2u,1u<<2,1u<<17};
    for(unsigned i=0;i<sizeof(flips)/sizeof(flips[0]);++i) {
        memcpy(bad,p,64); unsigned offset=word_index[i]*4;
        word(bad+offset,rsx_ao_word(bad+offset)^flips[i]);
        rows[0]=777; CHECK(!rsx_ao_position_rows(bad,64,rows)); CHECK(rows[0]==777);
    }
    memcpy(bad,p,64); word(bad,rsx_ao_word(bad)|(1u<<14)|(1u<<29));
    CHECK(!rsx_ao_position_rows(bad,64,rows));
    memcpy(bad,p,64); word(bad+12,rsx_ao_word(bad+12)|(1u<<14));
    CHECK(!rsx_ao_position_rows(bad,64,rows)); /* two position components */
    memcpy(bad,p,64); word(bad+20,rsx_ao_word(bad+20)^(1u<<12));
    CHECK(!rsx_ao_position_rows(bad,64,rows)); /* noncontiguous constant rows */
    const float scale[3]={1,1,.5f},offset[3]={0,0,.5f};
    rsx_ao_projection q;
    for(unsigned degrees=30;degrees<=100;degrees+=5) {
        double f=1/tan(degrees*3.141592653589793/360),n=.1,zfar=1000;
        float m[4][4]={{0}};
        m[0][0]=(float)(f/(16.0/9));m[1][1]=(float)f;
        m[2][2]=(float)((zfar+n)/(zfar-n));m[2][3]=(float)(-2*zfar*n/(zfar-n));m[3][2]=1;
        CHECK(rsx_ao_projection_extract(m,scale,offset,16.f/9,&q));
        CHECK(fabs(q.near_z-.1)<1e-6);
        CHECK(fabs(q.far_z-1000)<1); /* float far-plane precision */
        for(unsigned zi=1;zi<=100;zi+=7) {
            double d=(double)q.depth_a+(double)q.depth_b/zi;
            CHECK(fabs(q.depth_b/(d-q.depth_a)-zi)<1e-8);
        }
        float t=m[0][0];m[0][0]=m[0][2];m[0][2]=t;
        t=m[2][0];m[2][0]=m[2][2];m[2][2]=t;
        t=m[3][0];m[3][0]=m[3][2];m[3][2]=t;
        CHECK(rsx_ao_projection_extract(m,scale,offset,16.f/9,&q)); /* rotated basis */
    }
    /* Numeric camera fixture derived from the captured room, no guest code. */
    float m[4][4]={{.773098528f,0,.925981343f,59.8788185f},
        {.234595433f,2.12261844f,-.195862934f,-29.9323807f},
        {.759819031f,-.142512515f,-.634370327f,38.4294624f},
        {.759795845f,-.142508164f,-.634350955f,40.4282608f}};
    CHECK(rsx_ao_projection_extract(m,scale,offset,16.f/9,&q));
    CHECK(fabs(q.near_z-1)<.0001);
    CHECK(fabs(q.matrix[1][1]-1/tan(25*3.141592653589793/180))<1e-5);
    CHECK(!rsx_ao_projection_extract(m,scale,offset,32.f/9,&q)); /* storage width */
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) {
        float keep=m[r][c];m[r][c]=NAN;q.near_z=123;
        CHECK(!rsx_ao_projection_extract(m,scale,offset,16.f/9,&q));CHECK(q.near_z==123);m[r][c]=keep;
    }
    m[2][0]+=.01f;CHECK(!rsx_ao_projection_extract(m,scale,offset,16.f/9,&q));
    /* Uniformly scaled and translated model matrices retain the camera. */
    const float models[]={.01f,.5f,1,3,10,100};
    for(unsigned k=0;k<sizeof(models)/sizeof(models[0]);++k) {
        float s=models[k], a=1.02f,b=-2.02f,t=13.5f;
        float model[4][4]={{1.2f*s,0,0,7},{0,1.2f*s,0,-4},
                         {0,0,a*s,a*t+b},{0,0,s,t}};
        CHECK(rsx_ao_projection_extract(model,scale,offset,1,&q));
        CHECK(fabs(q.matrix[0][0]-1.2f)<1e-6);
        CHECK(fabs(q.matrix[1][1]-1.2f)<1e-6);
        CHECK(fabs(q.depth_b-b*.5)<2e-6);
        model[0][1]=.2f*s;
        CHECK(!rsx_ao_projection_extract(model,scale,offset,1,&q)); /* shear */
        model[0][1]=0;model[0][0]*=2;
        CHECK(!rsx_ao_projection_extract(model,scale,offset,1,&q)); /* unequal scale */
    }
    float effective[8], normalized[8]={1,1,.5f,0,0,0,.5f,0};
    for(unsigned i=0;i<20;++i) {
        float lo=i*.04f, hi=.9f;
        normalized[2]=.5f/(hi-lo);normalized[6]=(.5f-lo)/(hi-lo);
        CHECK(rsx_ao_viewport_transform(normalized,lo,hi,effective));
        CHECK(fabs(effective[2]-.5f)<1e-6 && fabs(effective[6]-.5f)<1e-6);
        for(unsigned d=0;d<11;++d) {
            float clip=d*.2f-1;
            float backend=(normalized[2]*clip+normalized[6])*(hi-lo)+lo;
            CHECK(fabs(backend-(effective[2]*clip+effective[6]))<1e-6);
        }
    }
    normalized[2]=10;normalized[6]=-9;
    CHECK(rsx_ao_viewport_transform(normalized,.95f,1,effective));
    CHECK(fabs(effective[2]-.5f)<1e-6 && fabs(effective[6]-.5f)<1e-6);
    const float invalid[][2]={{-1,1},{0,2},{1,0},{.5f,.5f},{0,NAN},{NAN,1}};
    for(unsigned i=0;i<6;++i) {
        effective[0]=123;
        CHECK(!rsx_ao_viewport_transform(normalized,invalid[i][0],invalid[i][1],effective));
        CHECK(effective[0]==123);
    }
    CHECK(!rsx_ao_viewport_transform(NULL,0,1,effective));
    CHECK(!rsx_ao_viewport_transform(normalized,0,1,NULL));
    normalized[3]=NAN;CHECK(!rsx_ao_viewport_transform(normalized,0,1,effective));
    float simple[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,-2},{0,0,1,0}};
    CHECK(rsx_ao_projection_extract(simple,scale,offset,1,&q));CHECK(isinf(q.far_z));
    simple[2][3]=2;CHECK(!rsx_ao_projection_extract(simple,scale,offset,1,&q));
    simple[2][3]=-2;simple[3][2]=0;CHECK(!rsx_ao_projection_extract(simple,scale,offset,1,&q));
    printf("AO projection: %u checks passed\n",checks);return 0;
}
