/* Exercise the production parked-list acknowledgement path. Only external
 * observation/coherence dependencies are inert; DMA list parsing and copies
 * come directly from spu_dma.h. Each process starts a fresh diagnostic cap. */
#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../spu_dma.h"

static uint8_t memory[0x40000];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
void (*g_spu_put_hook)(uint32_t, uint32_t);
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned long long ps3_ms_now(void) { return 0; }
unsigned spu_sn_defer_ticks(void) { return 0; }
unsigned* spu_ls_watch_list(int* n) { *n=0;return NULL; }
void spu_overlay_note_get(spu_context* c,uint32_t ea,const uint8_t* p,uint32_t n)
{ (void)c;(void)ea;(void)p;(void)n; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea,const void* c) { (void)ea;(void)c; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* c,uint32_t ea,const uint8_t* p,uint32_t n,const char* what)
{ (void)c;(void)ea;(void)p;(void)n;(void)what; }
void spu_ch_wake(spu_context* c) { (void)c; }

static void check(int ok,const char* what)
{ if(!ok){fprintf(stdout,"FAIL: %s\n",what);exit(2);} }
static int exercise(unsigned worker,unsigned iteration,int mode)
{
    spu_context* ctx=(spu_context*)calloc(1,sizeof(*ctx));
    check(ctx!=NULL,"allocate context");spu_context_init(ctx,worker);
    mfc_engine mfc;mfc_engine_init(&mfc);
    const uint32_t tag=5,bit=1u<<tag,other=1u<<31,ea=0x10000+worker*0x100;
    uint8_t pattern[32];for(unsigned i=0;i<32;i++)pattern[i]=(uint8_t)(iteration*3+i);
    memcpy(ctx->ls+0x1000,pattern,32);memset(memory+ea,0xA5,32);
    ctx->list_stall_mask=other;mfc.tag_completed&=~bit;
    spu_ls_write32(ctx,0x100,0x80000000u);spu_ls_write32(ctx,0x104,0);
    unsigned count=1;
    if(mode==1){
        count=2;spu_ls_write32(ctx,0x108,16);spu_ls_write32(ctx,0x10C,ea);
        if(iteration&1){memcpy(memory+ea,pattern,16);memset(ctx->ls+0x1000,0xCC,16);}
    }else if(mode==2){
        count=3;spu_ls_write32(ctx,0x108,0x80000010u);spu_ls_write32(ctx,0x10C,ea);
        spu_ls_write32(ctx,0x110,16);spu_ls_write32(ctx,0x114,ea+16);
    }
    const uint32_t cmd=mode==1 && (iteration&1)?0x40:0x20;
    check(mfc_run_list(ctx,0x100,count,0x1000,0,cmd,tag)==0,"initial list return");
    check(ctx->list_stall_mask==(other|bit),"real initial stall");
    check((ctx->event_status&2)!=0,"stall event raised");
    check(mfc_list_stall_ack(&mfc,ctx,tag)==0,"ack return");
    if(mode==2){
        check(ctx->list_stall_mask==(other|bit),"repark retains active tag");
        check(!(mfc.tag_completed&bit),"repark does not complete tag");
        check(ctx->list_stall_remaining[tag]==1,"repark remaining element");
        check(ctx->list_stall_dest_lsa[tag]==0x1010,"repark destination cursor");
        check(memcmp(memory+ea,pattern,16)==0,"first repark PUT bytes");
        check(memory[ea+16]==0xA5,"unexecuted second element");
        check(mfc_list_stall_ack(&mfc,ctx,tag)==0,"second ack return");
    }
    check(ctx->list_stall_mask==other,"ack clears only selected parked tag");
    check((mfc.tag_completed&bit)!=0,"drained tag completes");
    check(mfc.queue_count==0,"queue state preserved");
    if(mode==0){check(memcmp(ctx->ls+0x1000,pattern,32)==0,"empty LS unchanged");check(memory[ea]==0xA5,"empty VM unchanged");}
    if(mode==1){check(memcmp(ctx->ls+0x1000,pattern,16)==0,"GET/PUT LS bytes");check(memcmp(memory+ea,pattern,16)==0,"GET/PUT VM bytes");}
    if(mode==2)check(memcmp(memory+ea,pattern,32)==0,"both resumed PUT elements");
    free(ctx);return 0;
}
static volatile LONG arrived,go;
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg)
#else
static void* worker(void* arg)
#endif
{
    unsigned n=(unsigned)(uintptr_t)arg;InterlockedIncrement(&arrived);
    while(!InterlockedCompareExchange(&go,0,0))YieldProcessor();
    for(unsigned i=0;i<64;i++)exercise(n,i,0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
int main(int argc,char** argv)
{
    check(argc==2,"mode argument");memset(g_vm_page_bitmap,0xFF,sizeof g_vm_page_bitmap);
    if(strcmp(argv[1],"concurrent")==0){
#ifdef _WIN32
        HANDLE th[8];for(unsigned i=0;i<8;i++){th[i]=CreateThread(NULL,0,worker,(LPVOID)(uintptr_t)i,0,NULL);check(th[i]!=NULL,"start worker");}
#else
        pthread_t th[8];for(unsigned i=0;i<8;i++)check(pthread_create(&th[i],NULL,worker,(void*)(uintptr_t)i)==0,"start worker");
#endif
        while(InterlockedCompareExchange(&arrived,0,0)!=8)YieldProcessor();InterlockedExchange(&go,1);
        for(unsigned i=0;i<8;i++){
#ifdef _WIN32
            check(WaitForSingleObject(th[i],30000)==WAIT_OBJECT_0,"join worker");CloseHandle(th[i]);
#else
            check(pthread_join(th[i],NULL)==0,"join worker");
#endif
        }
    }else{
        int mode=strcmp(argv[1],"transfer")==0?1:strcmp(argv[1],"repark")==0?2:0;
        for(unsigned i=0;i<64;i++)exercise(0,i,mode);
    }
    puts("OK: actual parked/resume transfers, masks, completions and returns");return 0;
}
