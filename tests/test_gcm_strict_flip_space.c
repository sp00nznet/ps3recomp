/* CPU-only regression of the actual enqueue routine. No GPU or game data. */
#include "../libs/video/cellGcmSys.c"
uint8_t* vm_base;
uint32_t ppu_vm_size=0x200000;
uint32_t ppu_hle_inject_base;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width)
{ (void)ea;(void)value;(void)width; }
ps3_guest_caller_fn g_ps3_guest_caller=NULL; /* flip handler is not exercised */
#define CHECK(x) do {if(!(x)){fprintf(stderr,"flip space failed line %d\n",__LINE__);return 1;}}while(0)
/* Guest buffer-full callback stand-in: modes as in test_gcm_wait_flip.c. */
enum { CB_REFILL, CB_FAIL, CB_NO_SPACE };
static int s_mode, s_calls;
static u32 s_opd, s_ctx, s_count, s_target;
static s32 callback(u32 opd, u32 context, u32 count)
{
    s_calls++; s_opd = opd; s_ctx = context; s_count = count;
    if (s_mode == CB_REFILL) { vm_write32(context + 8, s_target); return 0; }
    if (s_mode == CB_FAIL) return (s32)0x80410001u;
    return 0;
}
int main(void)
{
#ifdef _WIN32
    _putenv_s("PS3_STRICT_BOOT","1");
#else
    setenv("PS3_STRICT_BOOT","1",1);
#endif
    vm_base=(uint8_t*)calloc(1,ppu_vm_size);CHECK(vm_base);
    memset(s_io_address_table,0xFF,sizeof(s_io_address_table));
    s_io_address_table[0]=0;s_io_address_table[1]=1;
    s_display_buffer_set[0]=1;
    const u32 ctx=0x10000,begin=0x100000,end=0x1FF000,cur=end-0x3E318;
    vm_write32(ctx,begin);vm_write32(ctx+4,end);vm_write32(ctx+8,cur);
    s_fifo_getoff=begin+GCM_RECYCLE_MARGIN+4;
    vm_write32(cur+4,0xA5A5A5A5);
    CHECK(gcm_flip_into_fifo(ctx,0)==1);
    CHECK(vm_read32(ctx+8)==cur+4 && vm_read32(cur)==GCM_FLIP_MARKER);
    CHECK(vm_read32(cur+4)==0xA5A5A5A5); /* No invented ring JUMP. */
    vm_write32(ctx+8,end-12);CHECK(gcm_flip_into_fifo(ctx,0)==1);
    CHECK(vm_read32(ctx+8)==end-8);
    CHECK(gcm_flip_into_fifo(ctx,0)==0 && vm_read32(ctx+8)==end-8);
    vm_write32(ctx+8,0xFFFFFFFC);CHECK(gcm_flip_into_fifo(ctx,0)==0);
    vm_write32(ctx+8,begin-4);CHECK(gcm_flip_into_fifo(ctx,0)==0);
    vm_write32(ctx+8,begin+1);CHECK(gcm_flip_into_fifo(ctx,0)==0);
    vm_write32(ctx+8,begin);s_io_address_table[1]=0xFFFF;
    CHECK(gcm_flip_into_fifo(ctx,0)==0);
    s_io_address_table[1]=1;
    /* Ring tail with the title's callback installed (RPCS3 gcmSetPrepareFlip):
     * callback(ctx, 8) runs once, then the marker goes at the refilled cursor. */
    const u32 opd=0x12340;
    vm_write32(ctx+12,opd);gcm_set_host_context_callback(callback);
    s_mode=CB_REFILL;s_target=begin+0x400;s_calls=0;
    vm_write32(ctx+8,end-8);vm_write32(end-8,0xA5A5A5A5);vm_write32(s_target+4,0xA5A5A5A5);
    CHECK(gcm_flip_into_fifo(ctx,0)==1);
    CHECK(s_calls==1 && s_opd==opd && s_ctx==ctx && s_count==8);
    CHECK(vm_read32(s_target)==GCM_FLIP_MARKER && vm_read32(ctx+8)==s_target+4);
    CHECK(vm_read32(s_target+4)==0xA5A5A5A5 && vm_read32(end-8)==0xA5A5A5A5);
    CHECK(vm_read32(ctx)==begin && vm_read32(ctx+4)==end);
    /* Exactly 12 bytes left needs no callback. */
    s_calls=0;vm_write32(ctx+8,end-12);
    CHECK(gcm_flip_into_fifo(ctx,0)==1 && s_calls==0 && vm_read32(ctx+8)==end-8);
    /* Failing callback: nothing enqueued, cursor untouched, result kept. */
    s_mode=CB_FAIL;s_calls=0;vm_write32(ctx+8,end-4);vm_write32(end-4,0xA5A5A5A5);
    CHECK(gcm_flip_into_fifo(ctx,0)==-1 && s_calls==1);
    CHECK(s_flip_callback_result==(s32)0x80410001u);
    CHECK(vm_read32(ctx+8)==end-4 && vm_read32(end-4)==0xA5A5A5A5);
    /* Callback that reports success without making space stays rejected. */
    s_mode=CB_NO_SPACE;s_calls=0;vm_write32(ctx+8,end-8);
    CHECK(gcm_flip_into_fifo(ctx,0)==0 && s_calls==1 && vm_read32(ctx+8)==end-8);
    /* Callback that moves current outside the ring is rejected. */
    s_mode=CB_REFILL;s_target=end+4;s_calls=0;vm_write32(ctx+8,end-8);
    CHECK(gcm_flip_into_fifo(ctx,0)==0 && s_calls==1);
    /* No title callback recorded in the context: still rejected, not invented. */
    vm_write32(ctx+12,0);s_calls=0;vm_write32(ctx+8,end-8);
    CHECK(gcm_flip_into_fifo(ctx,0)==0 && s_calls==0 && vm_read32(ctx+8)==end-8);
    free(vm_base);puts("strict flip: preserved guest ring, used title refill callback at ring tail, rejected invalid space");return 0;
}
