/* Real GCM FIFO + CPU blit probe, no game or GPU. Link like the existing
 * test_folklore_gcm_route fixture, replacing that fixture source with this one.
 * Run fresh processes with PS3_STRICT_BOOT=1:
 *   argb, argb-center, argb-live, rgb565, empty, self, local, main-local,
 *   local-main, main-pages -> exit 0;
 *   dither, subtract, invalid, convert, scaled, clipped, overlap,
 *   unknown-src-dma, unknown-dst-dma, local-wrap, local-dst-wrap,
 *   local-outside, local-dst-outside, main-unmapped, main-dst-unmapped,
 *   main-partial, main-hole, main-discontinuous -> exit 86.
 * The recording draw-engine substitute must never perform GPU work. */
#include "cellGcmSys.h"
#include "rsx_commands.h"
#include "rsx_draw_engine.h"
#include "rsx_live_draw.h"
#include "../runtime/memory/vm.h"
#include "../libs/video/rsx_nv3089_truncate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern void cellGcm_rsx_process_fifo(void);
extern u32 cellGcmResolveLocated(int local,u32 offset);
static u32 fifo_bytes,heap=0x00400000u;
static int live_on,positive_live,blits;
static const uint64_t guest_bytes=UINT64_C(0x100000000);
#define IO_ADDR 0x00100000u
#define SOURCE_OFFSET 0x10000u
#define DEST_OFFSET 0x20000u
#define SWIZZLE_OFFSET 0x30000u
#define CTRL (VM_HLE_INJECT_BASE+0x2000u)
/* Actual owned sparse guest VM: a 4 GiB address reservation, not a 4 GiB
 * committed allocation. Local tests access real bytes at configured VRAM EAs. */
static void allocate_vm(void) {
    assert(sizeof(void*)==8);
    ppu_vm_size=0; // Existing full-guest-address-space sentinel.
#ifdef _WIN32
    vm_base=VirtualAlloc(NULL,(SIZE_T)guest_bytes,MEM_RESERVE,PAGE_NOACCESS);assert(vm_base);
    assert(VirtualAlloc(vm_base,0x1000000u,MEM_COMMIT,PAGE_READWRITE)==vm_base);
    const uintptr_t inject=(uintptr_t)VM_HLE_INJECT_BASE&~(uintptr_t)65535u;
    assert(VirtualAlloc(vm_base+inject,0x20000u,MEM_COMMIT,PAGE_READWRITE)==vm_base+inject);
    assert(VirtualAlloc(vm_base+0xC0010000u,0x30000u,MEM_COMMIT,PAGE_READWRITE)==vm_base+0xC0010000u);
#else
    vm_base=mmap(NULL,(size_t)guest_bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(vm_base!=MAP_FAILED);
    assert(!mprotect(vm_base,0x1000000u,PROT_READ|PROT_WRITE));
    const uintptr_t inject=(uintptr_t)VM_HLE_INJECT_BASE&~(uintptr_t)65535u;
    assert(!mprotect(vm_base+inject,0x20000u,PROT_READ|PROT_WRITE));
    assert(!mprotect(vm_base+0xC0010000u,0x30000u,PROT_READ|PROT_WRITE));
#endif
}
static void release_vm(void) {
#ifdef _WIN32
    assert(VirtualFree(vm_base,0,MEM_RELEASE));
#else
    assert(!munmap(vm_base,(size_t)guest_bytes));
#endif
    vm_base=NULL;
}
static void write32(u32 a,u32 v) {
    u8* p=vm_base+a;p[0]=(u8)(v>>24);p[1]=(u8)(v>>16);p[2]=(u8)(v>>8);p[3]=(u8)v;
}
static u32 allocate(u32 size,u32 alignment) {
    heap=(heap+alignment-1)&~(alignment-1);u32 result=heap;heap+=size;return result;
}
static void emit(u32 method,u32 value) {
    write32(IO_ADDR+fifo_bytes,(1u<<18)|method);
    write32(IO_ADDR+fifo_bytes+4,value);fifo_bytes+=8;
}
int rsx_draw_engine_enabled(void) {return 0;}
void rsx_draw_engine_method(u32 a,u32 b) {(void)a;(void)b;abort();}
void rsx_draw_engine_flush(void) {abort();}
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void rsx_draw_engine_note_flip(u32 id) {(void)id;}
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e)
{(void)a;(void)b;(void)c;(void)d;(void)e;abort();}
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e)
{(void)a;(void)b;(void)c;(void)d;(void)e;abort();}
int rsx_live_draw_enabled(void) {return live_on;}
void rsx_live_draw_method(u32 method,u32 value) {(void)method;(void)value;}
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
int rsx_live_draw_blit(u32 sl,u32 so,u32 sp,u32 dl,u32 d,u32 dp,u32 w,u32 h) {
    assert(positive_live && sl==1 && dl==1 && so==SOURCE_OFFSET && sp==16);
    assert(d==DEST_OFFSET+32+4 && dp==32 && w==4 && h==2);++blits;
    return 0; // Recording substitute declines GPU ownership; CPU transfer is real.
}
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d)
{(void)a;(void)b;(void)c;(void)d;abort();}
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;abort();return 0;}

static void contract_edges(void) {
    rsx_nv3089_truncate s={1,0x313371C3u,3,10,3,0x100000u,0x100000u,0,
        0x20010u,0x20004u,0x10001u,0x20004u,0,0x40008u,32};
    unsigned bpp=0;
    assert(rsx_nv3089_truncate_check(&s,&bpp)==RSX_NV3089_EXACT && bpp==4);
    assert(rsx_nv3089_truncate_spans(&s,bpp,0x10000,0x20000,0x30000)==RSX_NV3089_EXACT);
    assert(rsx_nv3089_truncate_spans(&s,bpp,0xfffffff0u,0x20000,0x100000000ULL)==RSX_NV3089_BOUNDS);
    assert(rsx_nv3089_truncate_spans(&s,bpp,0x10000,0xfffffff0u,0x100000000ULL)==RSX_NV3089_BOUNDS);
    s.input_uv=1;assert(rsx_nv3089_truncate_check(&s,&bpp)==RSX_NV3089_SAMPLING);
    s.input_uv=0;s.context=0x31337A73u;assert(rsx_nv3089_truncate_check(&s,&bpp)==RSX_NV3089_CONTEXT);
    s.context=0x313371C3u;s.operation=4;assert(rsx_nv3089_truncate_check(&s,&bpp)==RSX_NV3089_OPERATION);
    s.operation=3;s.destination_pitch=8;assert(rsx_nv3089_truncate_check(&s,&bpp)==RSX_NV3089_PITCH);
}

int main(int argc,char** argv) {
    assert(argc==2);contract_edges();
    const char* mode=argv[1];
    const int rgb=!strcmp(mode,"rgb565"),empty=!strcmp(mode,"empty"),self=!strcmp(mode,"self");
    positive_live=!strcmp(mode,"argb-live");
    const int local=!strcmp(mode,"local"),main_local=!strcmp(mode,"main-local"),local_main=!strcmp(mode,"local-main");
    const int pages=!strcmp(mode,"main-pages") || !strcmp(mode,"main-hole") || !strcmp(mode,"main-discontinuous");
    const int positive=rgb || empty || self || positive_live || local || main_local || local_main ||
        !strcmp(mode,"main-pages") || !strcmp(mode,"argb-center") || !strcmp(mode,"argb");
    live_on=positive_live || !positive;
    const int overlap=!strcmp(mode,"overlap");
    const u32 bpp=rgb?2:4,source_pitch=pages?65520u:4*bpp,destination_pitch=(self || overlap)?source_pitch:32;
    const u32 height=pages?18u:2u;
    const u32 output_point=(self || overlap)?0:0x10001u;
    u32 conversion=1,source_format=rgb?7:3,destination_format=rgb?4:10;
    u32 dest_offset=self?SOURCE_OFFSET:DEST_OFFSET;
    u32 src_offset=pages?0x004FFFF0u:SOURCE_OFFSET;
    u32 source_dma=(local || local_main)?0xFEED0000u:0xFEED0001u;
    u32 destination_dma=(local || main_local)?0xFEED0000u:0xFEED0001u;
    if(!strcmp(mode,"dither"))conversion=0;
    if(!strcmp(mode,"subtract"))conversion=2;
    if(!strcmp(mode,"invalid"))conversion=0xffffffffu;
    if(!strcmp(mode,"convert"))destination_format=4;
    if(overlap)dest_offset=SOURCE_OFFSET+2;
    allocate_vm();
    cellGcmSetupContext(allocate(4,4),0x10000u,!strcmp(mode,"main-partial")?0x180000u:0x200000u,IO_ADDR,allocate,write32);
    if(pages) {
        assert(!cellGcmMapEaIoAddress(0x00800000u,0x00400000u,0x100000u));
        if(strcmp(mode,"main-hole"))assert(!cellGcmMapEaIoAddress(!strcmp(mode,"main-discontinuous")?0x00B00000u:0x00900000u,0x00500000u,0x100000u));
        assert(!cellGcmMapEaIoAddress(0x00A00000u,0x00600000u,0x100000u));
    }
    /* Initialize only legitimate source/destination fixture buffers. Invalid
     * command offsets below are never substituted with fabricated data. */
    const u32 source=local || local_main?0xC0000000u+SOURCE_OFFSET:
        (pages?0x008FFFF0u:cellGcmResolveLocated(0,SOURCE_OFFSET));
    const u32 destination=local || main_local?0xC0000000u+DEST_OFFSET:cellGcmResolveLocated(0,DEST_OFFSET);
    const u32 swizzle=cellGcmResolveLocated(0,SWIZZLE_OFFSET);
    assert(source && destination && swizzle);
    memset(vm_base+destination,0xa5,1024);memset(vm_base+swizzle,0x6d,256);
    for(u32 y=0;y<height;y++)for(u32 i=0;i<4*bpp;i++)vm_base[source+y*source_pitch+i]=(u8)((y*4*bpp+i)*37u+11u);
    u8 before[1024],expected[1024];memcpy(before,vm_base+destination,sizeof before);memcpy(expected,before,sizeof expected);
    if(!empty && !self)for(u32 y=0;y<height;y++)
        memcpy(expected+(y+1)*destination_pitch+bpp,vm_base+source+y*source_pitch,4*bpp);
    if(!strcmp(mode,"unknown-src-dma"))source_dma=0xDEADBEEFu;
    if(!strcmp(mode,"unknown-dst-dma"))destination_dma=0xDEADBEEFu;
    if(!strcmp(mode,"local-wrap")) {source_dma=0xFEED0000u;src_offset=0x50010000u;}
    if(!strcmp(mode,"local-dst-wrap")) {destination_dma=0xFEED0000u;dest_offset=0x50020000u;}
    if(!strcmp(mode,"local-outside")) {source_dma=0xFEED0000u;src_offset=0x0FFFFFF8u;}
    if(!strcmp(mode,"local-dst-outside")) {destination_dma=0xFEED0000u;dest_offset=0x0FFFFFF8u;}
    if(!strcmp(mode,"main-unmapped"))src_offset=0x00300000u;
    if(!strcmp(mode,"main-dst-unmapped"))dest_offset=0x00300000u;
    if(!strcmp(mode,"main-partial"))src_offset=0x0017FFF0u;
    /* Leave stale NV309E state bound elsewhere. Explicit linear context must
     * not let the legacy heuristic divert an ARGB 4x2 copy into Morton data. */
    emit(0x8300u,0x0102000Au);emit(0x8304u,SWIZZLE_OFFSET);
    emit(0x6184u,destination_dma);emit(0x6300u,destination_format);
    emit(0x6304u,destination_pitch<<16);emit(0x630Cu,dest_offset);
    emit(0xC184u,source_dma);emit(0xC198u,0x313371C3u);
    emit(0xC2FCu,conversion);emit(0xC300u,source_format);emit(0xC304u,3);
    emit(0xC308u,0);emit(0xC30Cu,empty?0:(!strcmp(mode,"clipped")?0x10001u:((height+2)<<16)|8u));
    emit(0xC310u,output_point);emit(0xC314u,(height<<16)|4u);
    emit(0xC318u,!strcmp(mode,"scaled")?0x80000u:0x100000u);emit(0xC31Cu,0x100000u);
    emit(0xC400u,(height<<16)|4u);emit(0xC404u,(!strcmp(mode,"argb-center")?0x10000u:0x20000u)|source_pitch);
    emit(0xC408u,src_offset);emit(0xC40Cu,0);
    write32(CTRL,fifo_bytes);cellGcm_rsx_process_fifo();
    if(!positive)return 99; // Strict rejection must have stopped this process.
    assert(blits==(positive_live?1:0));
    assert(!memcmp(vm_base+destination,expected,sizeof expected));
    for(unsigned i=0;i<256;i++)assert(vm_base[swizzle+i]==0x6d);
    if(self)for(u32 i=0;i<2*source_pitch;i++)assert(vm_base[source+i]==(u8)(i*37u+11u));
    release_vm();puts("NV3089 explicit TRUNCATE real-FIFO CPU transfer passed");return 0;
}
