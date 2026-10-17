#include "../libs/video/rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void reference(u8* d,u32 dp,const u8* src,u32 w,u32 h,const rsx_tex_layout* t,int rgba)
{
    for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x){
        const u8* s=src+(size_t)y*t->row_bytes+x*4u;u8* p=d+(size_t)y*dp+x*4u;
        if(rgba){p[0]=s[0];p[1]=s[1];p[2]=s[2];p[3]=s[3];}
        else {p[0]=s[1];p[1]=s[2];p[2]=s[3];p[3]=s[0];}
        if(t->base_format==0x9e)p[3]=255;
    }
}
static volatile u8 sink;
int main(int argc,char**argv)
{
    const u32 widths[]={1,2,3,7,15,16,17,31,63,127,256,1280,2560};
    const u32 heights[]={1,2,7,16},pads[]={0,1,7,16};
    for(u32 f=0;f<2;++f)for(u32 rgba=0;rgba<2;++rgba)
    for(u32 wi=0;wi<13;++wi)for(u32 hi=0;hi<4;++hi)for(u32 pi=0;pi<4;++pi){
        u32 w=widths[wi],h=heights[hi],sp=w*4+pads[pi],dp=w*4+pads[3-pi];
        size_t sn=(size_t)sp*h+32,dn=(size_t)dp*h+32;
        u8* src=malloc(sn),*a=malloc(dn),*b=malloc(dn);CHECK(src&&a&&b);
        for(size_t i=0;i<sn;++i)src[i]=(u8)(i*73u+(i>>8));memset(a,0x5a,dn);memset(b,0x5a,dn);
        rsx_tex_layout t;rsx_texture_layout_pitched(f?0xbe:0xa5,w,h,sp,&t);
        CHECK(!t.swizzled);reference(a+3,dp,src+1,w,h,&t,rgba);
        rsx_texture_decode(b+3,dp,src+1,w,h,&t,rgba);CHECK(!memcmp(a,b,dn));
        free(src);free(a);free(b);
    }
    /* Preserve the old byte-by-byte behavior even for overlapping buffers. */
    for(u32 f=0;f<2;++f)for(u32 rgba=0;rgba<2;++rgba)for(int delta=-8;delta<=8;++delta){
        u8 a[512],b[512];for(u32 i=0;i<512;++i)a[i]=b[i]=(u8)(i*37);
        rsx_tex_layout t;rsx_texture_layout_pitched(f?0xbe:0xa5,17,3,72,&t);
        reference(a+32+delta,72,a+32,17,3,&t,rgba);
        rsx_texture_decode(b+32+delta,72,b+32,17,3,&t,rgba);CHECK(!memcmp(a,b,512));
    }
    printf("PASS %u ARGB differential/padding/overlap checks\n",checks);
    if(argc>1&&!strcmp(argv[1],"--bench")){
        u32 w=2560,h=720;size_t n=(size_t)w*h*4;u8* src=malloc(n),*dst=malloc(n);CHECK(src&&dst);
        memset(src,0x69,n);rsx_tex_layout t;rsx_texture_layout_pitched(0xa5,w,h,w*4,&t);
        for(u32 r=0;r<5;++r){clock_t start=clock();for(u32 i=0;i<100;++i){rsx_texture_decode(dst,w*4,src,w,h,&t,0);sink^=dst[i];}
            printf("CPU 2560x720 ARGB decode %u: %.6f ms/frame\n",r,1000.0*(clock()-start)/CLOCKS_PER_SEC/100);}
        free(src);free(dst);
    }
    return 0;
}

