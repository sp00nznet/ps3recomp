/* Real runtime; temporary source instrumentation observes discovery work and
 * single-writer publication boundaries without adding production test hooks. */
#define main allsize_unused_mfc_main
#include "allsize_fixture.h"
#undef main

static spu_context* signature_ctx;
static int publication_checks;
static const uint8_t signature[16]={0xa1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static void lifted(spu_context* c){c->gpr[3]._u32[0]=42;}
static spu_context* fresh(void)
{
    spu_context* c=calloc(1,sizeof(*c));CHECK(c!=NULL);
    spu_context_init(c,0);c->image_id=501;return c;
}
void allsize_sig_published(void)
{
    if(!signature_ctx)return;
    /* A reader admitted immediately at count publication must not falsely
     * reject a signature whose flag was published too late. */
    signature_ctx->resident_ovl=0;
    spu_overlay_note_get(signature_ctx,0x77000040,signature_ctx->ls+0x20000,512);
    CHECK(signature_ctx->resident_ovl==777);++publication_checks;
}
static void absence_and_hits(spu_context* c)
{
    for(unsigned i=0;i<30;++i)spu_overlay_register_source(0x70000000+i*0x10000,600+i);
    spu_overlay_register_source(0,650);
    spu_overlay_register_source(0x70000000,651); /* first matching source wins */
    static const unsigned sizes[]={4,16,128,511,512,1024,16384};
    for(unsigned k=0;k<sizeof sizes/sizeof sizes[0];++k){
        allsize_scans=0;c->resident_ovl=17;
        spu_overlay_note_get(c,0x78000000,c->ls+0x20000,sizes[k]);
        CHECK(c->resident_ovl==17);CHECK(allsize_scans==0);
        allsize_scans=0;c->resident_ovl=0;
        spu_overlay_note_get(c,0x70000000,c->ls+0x20000,sizes[k]);
        CHECK(c->resident_ovl==600);CHECK(allsize_scans>0);
        c->resident_ovl=0;spu_overlay_note_get(c,0,c->ls+0x20000,sizes[k]);
        CHECK(c->resident_ovl==650);
    }
}
static void residency_and_epochs(spu_context* c)
{
    const uint32_t base=0x10000,source=0x71000000;
    static const uint8_t code[]={0x11,2,3,4,5,6,7,8};
    const spu_overlay_code_segment seg={base,sizeof code,code};
    spu_begin_image(701);spu_register_function(base,lifted);spu_begin_image(0);
    CHECK(spu_overlay_register_verified_region(source,base,0x400,701,&seg,1));
    memcpy(c->ls+base,code,sizeof code);
    spu_overlay_note_get(c,source,c->ls+base,512);
    CHECK(spu_lifted_lookup(c,base)==lifted);
    CHECK(c->resident_code[0].verify_state==SPU_OVERLAY_VERIFIED);
    uint32_t epoch=c->dcache_residency;
    /* A continuation's EA is not a registered source: negative discovery
     * must still reset verification, and changed code must fail validation. */
    c->ls[base+4]^=0xff;allsize_scans=0;
    spu_overlay_note_get(c,source+4,c->ls+base+4,512);
    CHECK(allsize_scans==0);CHECK(c->dcache_residency!=epoch);
    CHECK(c->resident_code[0].verify_state==SPU_OVERLAY_PENDING);
    CHECK(spu_lifted_lookup(c,base)==NULL);
    memcpy(c->ls+base,code,sizeof code);
    spu_overlay_note_get(c,source,c->ls+base,512);
    CHECK(spu_lifted_lookup(c,base)==lifted);
    /* Partial foreign overwrite starts below the resident range. */
    epoch=c->dcache_residency;
    spu_overlay_note_get(c,0x79000000,c->ls+base-16,512);
    CHECK(!c->resident_code[0].image_id);CHECK(c->dcache_residency!=epoch);


}
static void old_matcher_edges(spu_context* c)
{
    /* An early exact-EA hit with a wrong fixed load must continue to the
     * later exact-EA hit, rather than adopting the first indexed source. */
    static const uint8_t code[]={1,2,3,4};
    const spu_overlay_code_segment a={0x24000,4,code},b={0x25000,4,code};
    CHECK(spu_overlay_register_verified_region(0x72000000,0x24000,0x400,710,&a,1));
    CHECK(spu_overlay_register_verified_region(0x72000000,0x25000,0x400,711,&b,1));
    memset(c->resident_code,0,sizeof c->resident_code);
    spu_overlay_note_get(c,0x72000000,c->ls+0x25000,512);
    CHECK(c->resident_code[0].image_id==711);
    spu_overlay_register_region(0x73000000,0x400,712);
    memset(c->resident_code,0,sizeof c->resident_code);c->resident_ovl=19;
    spu_overlay_note_get(c,0x73000000,c->ls+SPU_LS_SIZE-0x100,16);
    CHECK(!c->resident_code[0].image_id && c->resident_ovl==19);
    for(unsigned i=0;i<4;++i){c->resident_code[i].image_id=800+i;c->resident_code[i].lsa=0x3000+i*0x100;c->resident_code[i].size=16;}
    spu_overlay_note_get(c,0x73000000,c->ls+0x27000,512);
    for(unsigned i=0;i<4;++i)CHECK(c->resident_code[i].image_id==(int)(800+i));
    memset(c->resident_code,0,sizeof c->resident_code);
}
static void list_resume(spu_context* c)
{
    memset(guest_memory+0x21000,0x51,512);memset(guest_memory+0x22000,0x62,512);
    spu_ls_write32(c,0x3000,512);spu_ls_write32(c,0x3004,0x21000);
    spu_ls_write32(c,0x3008,0x80000000);spu_ls_write32(c,0x300c,0);
    spu_ls_write32(c,0x3010,512);spu_ls_write32(c,0x3014,0x22000);
    mfc_engine* m=mfc_for(c);m->tag_completed&=~(1u<<4);
    c->mfc_lsa=0x28000;c->mfc_eal=0x3000;c->mfc_size=24;c->mfc_tag=4;
    CHECK(mfc_submit(m,c,0x44)==0);
    CHECK(c->ls[0x28000]==0x51 && c->ls[0x28200]==0);
    CHECK(c->list_stall_mask==(1u<<4) && !(m->tag_completed&(1u<<4)));
    CHECK(c->event_status&2);CHECK(c->list_stall_dest_lsa[4]==0x28200);
    /* Resume must read this modified remaining element live. */
    spu_ls_write32(c,0x3014,0x21000);
    CHECK(mfc_list_stall_ack(m,c,4)==0);
    CHECK(c->ls[0x28200]==0x51 && !c->list_stall_mask);
    CHECK(m->tag_completed&(1u<<4));
}
static void signatures(spu_context* c)
{
    memcpy(c->ls+0x20000,signature,16);signature_ctx=c;
    spu_overlay_register_sig(signature,777);signature_ctx=NULL;
    CHECK(publication_checks==1);
    c->resident_ovl=19;spu_overlay_note_get(c,0x77000040,c->ls+0x20000,511);
    CHECK(c->resident_ovl==19);
    spu_overlay_note_get(c,0x77000040,c->ls+0x20000,512);
    CHECK(c->resident_ovl==777);
    CHECK(spu_overlay_match_sig(signature)==777);
    /* Known EA and signature both match: registration order remains exact. */
    spu_overlay_note_get(c,0x70000000,c->ls+0x20000,512);
    CHECK(c->resident_ovl==600);
}
int main(int argc,char** argv)
{
    spu_context* c=fresh();
    if(argc>1 && !strcmp(argv[1],"full")){
        for(unsigned i=0;i<128;++i)spu_overlay_register_source(0x70000000+i*0x10000,600+i);
        unsigned count=s_ovl_src_count;
        /* Rejection must occur before reading the inaccessible signature. */
        spu_overlay_register_sig((const uint8_t*)(uintptr_t)1,999);
        CHECK(s_ovl_src_count==(int)count);
        allsize_scans=0;c->resident_ovl=19;
        spu_overlay_note_get(c,0x78000000,c->ls+0x20000,512);
        CHECK(c->resident_ovl==19);CHECK(allsize_scans==0);
    }else{
        absence_and_hits(c);residency_and_epochs(c);old_matcher_edges(c);
        list_resume(c);signatures(c);
    }
    spu_mfc_release(c);free(c);
    printf("OVERLAY_ALLSIZE passed=%d failed=%d\n",g_pass,g_fail);
    return g_fail?1:0;
}
