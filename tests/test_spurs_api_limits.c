/* cellSpurs argument limits. Manufactured policy module only, no game code. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"

void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n,f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart*1000/f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
void sys_event_queue_cancel_by_id(u32 id) {(void)id;}
void sys_event_queue_uncancel_by_id(u32 id) {(void)id;}

static atomic_uint gate;
static void hold_policy(spu_context* c)
{
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    while(!atomic_load(&gate)) Sleep(1);
    vm_write8(ea+0x80+wid,3);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void finalize(u32 ea)
{
    for(unsigned i=0;i<3000;i++) {
        s32 rc=cellSpursFinalize((CellSpurs*)(uintptr_t)ea);
        if (!rc) return;
        assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY);Sleep(1);
    }
    assert(!"finalize failed");
}

int main(void)
{
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    memset(vm_base+0x9000,0x11,16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+0x9000,16),hold_policy,77,"api limits fixture");
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);

    /* maxContention is a u8 in the instance; 256 must not wrap to 0 ("none"). */
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,256)==0);
    assert(vm_read32(0x8000)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==CELL_SPURS_MAX_SPU);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,300)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==CELL_SPURS_MAX_SPU);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,3)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==3);
    puts("SPURS maxContention above 8 clamps instead of wrapping PASS");

    /* RequestIdleSpu on a slot that is not in use must fail and leave it alone. */
    assert((u32)cellSpursRequestIdleSpu(0x1000,5,2)==CELL_SPURS_CORE_ERROR_SRCH);
    assert(vm_read8(0x1000+SPURS_WKL_IDLE2+5)==0);
    assert((u32)cellSpursRequestIdleSpu(0x1000,16,2)==CELL_SPURS_CORE_ERROR_INVAL);
    assert(cellSpursRequestIdleSpu(0x1000,0,2)==0);
    assert(vm_read8(0x1000+SPURS_WKL_IDLE2)==2);
    puts("SPURS RequestIdleSpu rejects a workload slot that is not in use PASS");

    atomic_store(&gate,1);
    finalize(0x1000);
    free(vm_base);return 0;
}
