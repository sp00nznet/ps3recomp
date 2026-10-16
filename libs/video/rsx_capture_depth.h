#ifndef RSX_CAPTURE_DEPTH_H
#define RSX_CAPTURE_DEPTH_H
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Diagnostic raw samples, preserving signed zero, infinities and NaN payloads.
 * Explicit little-endian IEEE754 bytes; no normalization or sample averaging. */
#define RSX_CAPTURE_DEPTH_MAX_BYTES (16u*1024u*1024u)
static int rsx_capture_depth_write(FILE* file,const float* values,size_t count)
{
    _Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24 && FLT_MAX_EXP==128,
                   "depth capture requires binary32 float");
    if(!file || (!values && count) || count>RSX_CAPTURE_DEPTH_MAX_BYTES/4u)return -1;
    unsigned char bytes[4096];
    while(count) {
        size_t n=count>sizeof(bytes)/4?sizeof(bytes)/4:count;
        for(size_t i=0;i<n;++i) {
            uint32_t bits;memcpy(&bits,values+i,4);
            for(unsigned j=0;j<4;++j)bytes[i*4+j]=(unsigned char)(bits>>(j*8));
        }
        if(fwrite(bytes,4,n,file)!=n)return -1;
        values+=n;count-=n;
    }
    return 0;
}
#endif
