#include "rsx_fsr1_constants.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(rsx_fsr1_constants)==80, "push block size");
_Static_assert(offsetof(rsx_fsr1_constants,rcas)==64, "RCAS offset");
static float as_float(uint32_t u) {float f;memcpy(&f,&u,4);return f;}
int main(void)
{
    const unsigned sizes[][4]={{1,1,1,1},{1,1,2,2},{1280,720,1920,1080},
        {1920,1080,3840,2160},{2560,1440,3840,2160},{1720,720,3440,1440},
        {853,479,1706,958},{1920,1080,1920,1080}};
    unsigned checks=0;
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        const unsigned *s=sizes[i];
        for(unsigned stop=0;stop<3;++stop) {
            rsx_fsr1_constants c;
            assert(!rsx_fsr1_make_constants(&c,s[0],s[1],s[2],s[3],(float)stop));
            for(unsigned axis=0;axis<2;++axis) {
                const double ratio=(double)s[axis]/s[axis+2];
                const double scale=as_float(c.easu[0][axis]);
                const double offset=as_float(c.easu[0][axis+2]);
                const double inverse=as_float(c.easu[1][axis]);
                assert(fabs(scale-ratio)<1e-6);
                assert(fabs(inverse*s[axis]-1)<1e-6);
                /* Both endpoint pixel centers must match full-image resampling. */
                for(unsigned edge=0;edge<2;++edge) {
                    const unsigned pixel=edge?s[axis+2]-1:0;
                    const double mapped=(pixel*scale+offset+0.5)*inverse;
                    assert(fabs(mapped-((double)pixel+0.5)/s[axis+2])<1e-6);
                    ++checks;
                }
            }
            assert(fabsf(as_float(c.rcas[0])-exp2f(-(float)stop))<1e-6f);
            assert(c.rcas[2]==0 && c.rcas[3]==0);
            assert(c.easu[3][2]==0 && c.easu[3][3]==0);
            ++checks;
        }
    }
    const float bad_sharp[]={-1.f,2.01f,NAN,INFINITY};
    rsx_fsr1_constants c,before;memset(&c,0xA5,sizeof(c));before=c;
    for(unsigned i=0;i<4;++i) {
        assert(rsx_fsr1_make_constants(&c,100,100,200,200,bad_sharp[i])==-1);
        assert(!memcmp(&c,&before,sizeof(c)));++checks;
    }
    const unsigned bad[][4]={{0,1,1,1},{1,0,1,1},{1,1,0,1},{1,1,1,0},
        {2,1,1,1},{1,2,1,1},{1,1,16777217,1},{1,1,1,16777217}};
    for(unsigned i=0;i<8;++i) {
        assert(rsx_fsr1_make_constants(&c,bad[i][0],bad[i][1],bad[i][2],bad[i][3],1)==-1);
        assert(!memcmp(&c,&before,sizeof(c)));++checks;
    }
    assert(rsx_fsr1_make_constants(NULL,1,1,1,1,1)==-1);++checks;
    printf("FSR1 constant mapping and rejection checks passed: %u\n",checks);
}
