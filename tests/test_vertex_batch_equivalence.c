/* Standalone: clang -O2 -Iinclude -Ilibs/video tests/test_vertex_batch_equivalence.c
 * libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c -o batch-test.exe */
#include "../libs/video/rsx_vertex_compact.h"
#include "../libs/video/rsx_vertex_formats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
static unsigned checks;
#define CHECK(c) do {++checks;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static u32 rng=0x9178ab65u;
static u32 next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static u8 memory[4][8192];
static const u8* resolve(void* u,u32 location,u32 offset,u32 bytes)
{(void)u;return location<4 && offset<=8192 && bytes<=8192-offset ? memory[location]+offset : NULL;}
static int reference(const rsx_vertex_fetch_plan* p,const rsx_vertex_ref* refs,u32 n,u8* out)
{for(u32 i=0;i<n;++i)if(!rsx_vertex_fetch_one(p,refs+i,out+(size_t)i*p->layout.stride))return 0;return 1;}
static void init(rsx_vertex_fetch_plan* p)
{
    memset(p,0,sizeof(*p));rsx_vertex_layout_plan_init(&p->layout,15);
    p->guest_ptr=resolve;
    for(u32 a=0;a<4;++a){
        rsx_vertex_fetch_attr* f=&p->attr[a];
        f->desc.type=a==1?RSX_VTX_TYPE_HALF:a==2?RSX_VTX_TYPE_UNORM8:RSX_VTX_TYPE_FLOAT;
        f->desc.size=4;f->desc.location=a;f->stride=16;f->elem_size=a==1?8:a==2?4:16;
        for(u32 k=0;k<4;++k){u32 bits=next();memcpy(f->default_value+k,&bits,4);}
    }
}
static void compare(rsx_vertex_fetch_plan* p,rsx_vertex_ref* refs,u32 n)
{
    u8 a[257*64],b[257*64];memset(a,0xa5,sizeof(a));memset(b,0xa5,sizeof(b));
    rsx_vertex_fetch_plan_prepare(p,refs,n);
    int ra=reference(p,refs,n,a),rb=rsx_vertex_fetch_many(p,refs,n,b);
    CHECK(ra==rb);CHECK(!memcmp(a,b,sizeof(a)));
}
static void guards(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;GetSystemInfo(&si);size_t page=si.dwPageSize;
    u8* m=VirtualAlloc(NULL,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(m);
    DWORD old;CHECK(VirtualProtect(m+page,page,PAGE_NOACCESS,&old));
    for(u32 type=2;type<=4;++type)for(u32 size=1;size<=4;++size){
        rsx_vertex_fetch_plan p;memset(&p,0,sizeof(p));rsx_vertex_layout_plan_init(&p.layout,1);
        u32 bytes=size*(type==2?4:type==3?2:1);u8* src=m+page-bytes;
        for(u32 j=0;j<bytes;++j)src[j]=(u8)next();
        p.attr[0].desc.type=type;p.attr[0].desc.size=size;p.attr[0].stable_base=src;
        rsx_vertex_ref refs[2]={{0,0},{0,0}};u8 a[32],b[32];
        CHECK(reference(&p,refs,2,a));CHECK(rsx_vertex_fetch_many(&p,refs,2,b));CHECK(!memcmp(a,b,32));
    }
    CHECK(VirtualFree(m,0,MEM_RELEASE));
#endif
}
static volatile u32 sink;
static double bench(int batch,rsx_vertex_fetch_plan* p,rsx_vertex_ref* refs,u8* out)
{
    clock_t start=clock();
    for(u32 j=0;j<3000;++j){
        int ok=batch?rsx_vertex_fetch_many(p,refs,512,out):reference(p,refs,512,out);
        u32 bits;memcpy(&bits,out+(j%512)*64,4);sink^=bits+(u32)ok;
    }
    return (double)(clock()-start)/CLOCKS_PER_SEC;
}
int main(int argc,char** argv)
{
    for(u32 a=0;a<4;++a)for(u32 j=0;j<8192;++j)memory[a][j]=(u8)next();
    for(u32 trial=0;trial<12000;++trial){
        rsx_vertex_fetch_plan p;init(&p);rsx_vertex_ref refs[257];
        for(u32 i=0;i<257;++i){refs[i].vertex_id=next()%256;refs[i].base_index=next()%64;}
        if(trial&1)for(u32 a=0;a<4;++a){
            p.attr[a].desc.type=next()%10;p.attr[a].desc.size=next()%6;
            p.attr[a].desc.frequency=next()%5;p.attr[a].elem_size=16;
        }
        p.divider_mask=next()&15;
        if(trial%5==0)p.guest_ptr=NULL;
        if(trial%7==0)refs[13].vertex_id=0xfffffu;
        if(trial%11==0){p.inline_data=memory[0];p.inline_stride=64;p.inline_bytes=8192;for(u32 a=0;a<4;++a)p.inline_off[a]=a*16;}
        compare(&p,refs,trial%258);
    }
    guards();CHECK(!rsx_vertex_fetch_many(NULL,NULL,0,NULL));
    rsx_vertex_fetch_plan p;init(&p);CHECK(rsx_vertex_fetch_many(&p,NULL,0,NULL));
    CHECK(!rsx_vertex_fetch_many(&p,NULL,1,NULL));
    printf("PASS %u differential/boundary checks\n",checks);
    if(argc>1 && !strcmp(argv[1],"--bench")){
        rsx_vertex_ref refs[512];u8 out[512*64];
        for(u32 i=0;i<512;++i){refs[i].vertex_id=(i*73)%512;refs[i].base_index=0;}
        init(&p);rsx_vertex_fetch_plan_prepare(&p,refs,512);
        for(u32 r=0;r<5;++r){double a,b;if(r&1){b=bench(1,&p,refs,out);a=bench(0,&p,refs,out);}else{a=bench(0,&p,refs,out);b=bench(1,&p,refs,out);}
            printf("CPU mixed batch %u: reference %.6f candidate %.6f ratio %.3f\n",r,a,b,b/a);}
    }
    return 0;
}
