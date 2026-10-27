/* Synthetic storage-control commands, real channels/interpreter and reservations. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t guest[1u << 20];
uint8_t* vm_base = guest;
uint32_t ppu_vm_size;
#include "../spu_channels.c"
#include "../spu_interp.h"
static spu_context writer, peer, first_peer;
static uint8_t expected_ls[SPU_LS_SIZE];
static void be32(uint8_t* p, uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void prepare(uint32_t ea, uint32_t n) {
    memset(guest+ea-128,0x77,n+256);
    memset(writer.ls,0xA5,SPU_LS_SIZE);
    memcpy(expected_ls,writer.ls,SPU_LS_SIZE);
    writer.mfc_lsa=SPU_LS_SIZE-1; /* No valid DMA span; SDCRZ does not use LSA. */
    writer.mfc_eah=0;writer.mfc_eal=ea;writer.mfc_size=n;writer.mfc_tag=11;
    mfc_for(&writer)->tag_completed &= ~(1u<<11);
}
static void cleared(uint32_t ea,uint32_t n) {
    for(uint32_t i=0;i<n;i++)assert(guest[ea+i]==0);
    assert(guest[ea-1]==0x77 && guest[ea+n]==0x77);
    assert(!memcmp(writer.ls,expected_ls,SPU_LS_SIZE));
    spu_wrch(&writer,MFC_WrTagMask,spu_splat_u32(1u<<11));
    spu_wrch(&writer,MFC_WrTagUpdate,spu_splat_u32(2));
    assert(spu_rdch(&writer,MFC_RdTagStat)._u32[0]==(1u<<11));
}
static void reserve(uint32_t ea) {
    peer.mfc_eal=ea;peer.mfc_eah=0;peer.mfc_lsa=0x2000;
    assert(spu_mfc_atomic(&peer,MFC_GETLLAR_CMD));assert(peer.resv_valid);
    peer.event_status=0;
}
int main(void) {
    const uint32_t ea=0x20000;
    spu_context_init(&writer,0);writer.image_id=-1;
    spu_context_init(&peer,0);peer.image_id=-1;
    spu_context_init(&first_peer,0);first_peer.image_id=-1;
    prepare(ea,2048);
    spu_wrch(&writer,MFC_EAL,spu_splat_u32(ea));
    spu_wrch(&writer,MFC_Size,spu_splat_u32(2048));
    spu_wrch(&writer,MFC_TagID,spu_splat_u32(11));
    spu_wrch(&writer,MFC_Cmd,spu_splat_u32(0x89));
    cleared(ea,2048);
    prepare(ea,2048);
    /* Upper halfword is replacement/transfer class; lower16 is opcode. */
    spu_wrch(&writer,MFC_Cmd,spu_splat_u32(0x01020089u));cleared(ea,2048);
    /* Independently encode synthetic wrch instructions with preloaded registers. */
    prepare(ea,16384);
    const unsigned channels[]={MFC_EAL,MFC_Size,MFC_TagID,MFC_Cmd};
    const uint32_t values[]={ea,16384,11,0x89};
    for(unsigned i=0;i<4;i++) {
        writer.gpr[10+i]=spu_splat_u32(values[i]);
        be32(writer.ls+0x1000+4*i,0x21A00000u|(channels[i]<<7)|(10+i));
    }
    be32(writer.ls+0x1010,0);memcpy(expected_ls,writer.ls,SPU_LS_SIZE);
    spu_interp_run(&writer,0x1000);
    cleared(ea,16384);
    /* Zero-to-zero writes must notify peers on every touched line. Master
     * tracks SPU reservations; it does not expose PPU reservation epochs. */
    prepare(ea,2048);memset(guest+ea,0,2048);reserve(ea+1920);
    first_peer.mfc_eal=ea;first_peer.mfc_eah=0;first_peer.mfc_lsa=0x2000;
    assert(spu_mfc_atomic(&first_peer,MFC_GETLLAR_CMD));
    assert(first_peer.resv_valid);first_peer.event_status=0;
    unsigned long notifications=g_spu_lr_raise;
    assert(mfc_submit(mfc_for(&writer),&writer,0x89)==0);
    assert(g_spu_lr_raise==notifications+2);
    assert(!first_peer.resv_valid && (first_peer.event_status&SPU_EVENT_LR));
    assert(!peer.resv_valid && (peer.event_status&SPU_EVENT_LR));
    be32(peer.ls+0x2000,0xABCDEF01);assert(spu_mfc_atomic(&peer,MFC_PUTLLC_CMD));
    assert(peer.atomic_stat==1 && guest[ea+1920]==0);
    reserve(ea+2048);assert(mfc_submit(mfc_for(&writer),&writer,0x89)==0);
    assert(peer.resv_valid && !(peer.event_status&SPU_EVENT_LR));
    /* Unsupported storage-control forms fail without guest writes or a
     * successful tag receipt, including high EA bits instead of truncation. */
    const struct {uint64_t ea; uint32_t size;} invalid[]={
        {ea,0},{ea,16512},{ea+1,2048},{ea,16},
        {0x100020000ull,2048},{0xFFFFFF80ull,256}
    };
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        prepare(ea,2048);writer.mfc_eah=(uint32_t)(invalid[i].ea>>32);
        writer.mfc_eal=(uint32_t)invalid[i].ea;writer.mfc_size=invalid[i].size;
        assert(mfc_submit(mfc_for(&writer),&writer,0x89)==-1);
        for(unsigned j=0;j<2048;j++)assert(guest[ea+j]==0x77);
        assert(!(mfc_for(&writer)->tag_completed&(1u<<11)));
        assert(mfc_submit(mfc_for(&writer),&writer,0x01020089u)==-1);
        assert(!(mfc_for(&writer)->tag_completed&(1u<<11)));
    }
    prepare(ea,2048);uint8_t* real_vm=vm_base;vm_base=NULL;
    assert(mfc_submit(mfc_for(&writer),&writer,0x89)==-1);vm_base=real_vm;
    assert(guest[ea]==0x77 && !(mfc_for(&writer)->tag_completed&(1u<<11)));
    /* An earlier stall in the same tag must prevent zeroing/completion. An
     * unrelated stalled tag must not block this tag's synchronous command. */
    prepare(ea,2048);writer.list_stall_mask=(1u<<11);
    assert(mfc_submit(mfc_for(&writer),&writer,0x89)==-1);
    assert(guest[ea]==0x77 && !(mfc_for(&writer)->tag_completed&(1u<<11)));
    writer.list_stall_mask=(1u<<12);
    assert(mfc_submit(mfc_for(&writer),&writer,0x89)==0);cleared(ea,2048);
    writer.list_stall_mask=0;
    spu_coh_unregister(&first_peer);spu_coh_unregister(&peer);
    spu_mfc_release(&first_peer);spu_mfc_release(&peer);spu_mfc_release(&writer);
    puts("PASS: SDCRZ real channels/encoded interpreter, full zero range, no LS operand, peer reservation invalidation and zero-to-zero notifications, unaffected neighbor");
    return 0;
}

void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
