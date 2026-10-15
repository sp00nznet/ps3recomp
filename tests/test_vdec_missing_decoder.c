/* Build without PS3RECOMP_FFMPEG_RUNTIME to test the real unavailable path.
 * Run again with PS3_STRICT_BOOT=1 and require exit 86 plus the fatal category. */
#include <stdint.h>
#include <stdio.h>
#include "../libs/codec/cellVdec.c"

uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
uint32_t ppu_hle_inject_base;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width)
{ (void)ea;(void)value;(void)width; }

int main(void)
{
    uint8_t memory[0x20000]={0};vm_base=memory;
    vm_write32(0x10000,CELL_VDEC_CODEC_TYPE_AVC);
    vm_write32(0x10010,0xDEADBEEF);
    /* Repeat beyond slot capacity: a failed open must not leak a slot. */
    for (int n=0;n<MAX_VDEC+1;++n) {
        s32 result=cellVdecOpen((const CellVdecType*)(uintptr_t)0x10000,
            NULL,NULL,(CellVdecHandle*)(uintptr_t)0x10010);
        if ((u32)result!=CELL_VDEC_ERROR_FATAL || vm_read32(0x10010)!=0xDEADBEEF)
            return 1;
        for (int i=0;i<MAX_VDEC;++i) if(s_vdec[i].in_use) return 2;
    }
    puts("missing decoder: failure preserved output and released slots");
    return 0;
}
