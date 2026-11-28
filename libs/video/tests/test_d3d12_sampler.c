#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
typedef uint32_t u32;
typedef uint8_t u8;
#include "rsx_d3d12_sampler.h"
int main(void)
{
 D3D12_SAMPLER_DESC d;
 /* Forest vegetation mask: mip filtering is enabled, but the guest clamps
  * both ends to base level. The old unbounded MaxLOD produced black fringes. */
 u32 k[4]={0,0x01032000,0x80000000,0};
 rsx_sampler_desc(k,&d);
 assert(d.MinLOD==0 && d.MaxLOD==0);
 assert(d.Filter==D3D12_FILTER_MIN_MAG_MIP_POINT);
 /* Its colour atlas uses trilinear, a negative bias, and levels 0..9. */
 k[0]=0x40010101;k[1]=0x02063F00;k[2]=0x80048000;
 rsx_sampler_desc(k,&d);
 assert(d.MinLOD==0 && d.MaxLOD==9 && d.MipLODBias==-1);
 assert(d.Filter==D3D12_FILTER_MIN_MAG_MIP_LINEAR);
 /* Fractional LOD endpoints must retain the guest's 8 fractional bits. */
 k[2]=0x80000000u | (384u<<19) | (704u<<7);
 rsx_sampler_desc(k,&d);assert(d.MinLOD==1.5f && d.MaxLOD==2.75f);
 k[1]=0x02022000;rsx_sampler_desc(k,&d);
 assert(d.MinLOD==1.5f && d.MaxLOD==1.5f);
 k[1]=0x02062000;k[2]=0x80000000u | (512u<<19) | (256u<<7);
 rsx_sampler_desc(k,&d);assert(d.MinLOD==2 && d.MaxLOD==2);
 return 0;
}
