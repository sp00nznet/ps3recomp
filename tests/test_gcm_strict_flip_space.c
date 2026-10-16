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
#define CHECK(x) do {if(!(x)){fprintf(stderr,"flip space failed line %d\n",__LINE__);return 1;}}while(0)
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
    free(vm_base);puts("strict flip: preserved guest ring and rejected invalid space");return 0;
}
