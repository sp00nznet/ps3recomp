/* Manufactured policy modules exercise the real initializer, scheduler and
 * SPU policy execution. No game code, addresses or completion substitutions. */
#define spu_workload_dispatch_async fixture_unbounded_dispatch
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"

void spu_indirect_branch(spu_context*);
void spu_halt(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n,f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart*1000/f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
static atomic_uint canceled_queues;
void sys_event_queue_cancel_by_id(u32 id) { assert(id==3 || id==4);atomic_fetch_or(&canceled_queues,1u<<id); }
void sys_event_queue_uncancel_by_id(u32 id) {(void)id;}
/* Observe the legacy bypass without substituting any policy completion. */
static atomic_uint unbounded_dispatches;
int fixture_unbounded_dispatch(const uint8_t* pm,uint32_t size,uint32_t data)
{ (void)pm;(void)size;(void)data;atomic_fetch_add(&unbounded_dispatches,1);return 0; }
static atomic_uint entered, produced, finished, release_consumer;
static atomic_uint consumer_lane,producer_lane;
static void finish(spu_context* c)
{
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    /* The manufactured guest removes itself from the runnable set. */
    vm_write8(ea+0x80+wid,3);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void consumer(spu_context* c)
{
    spu_lockline_lock();spu_coh_reserve(c,spu_ls_read32(c,0x1C4));spu_lockline_unlock();
    atomic_store(&consumer_lane,spu_ls_read32(c,0x1C8));
    atomic_fetch_add(&entered,1);
    while (!atomic_load(&produced) && !atomic_load(&release_consumer)) Sleep(1);
    assert(atomic_load(&produced));
    atomic_fetch_add(&finished,1);finish(c);
}
static void producer(spu_context* c)
{
    atomic_store(&producer_lane,spu_ls_read32(c,0x1C8));
    atomic_store(&produced,1);finish(c);
}
static void wait_for(atomic_uint* p,unsigned value,const char* what)
{
    for(unsigned i=0;i<3000;i++) { if(atomic_load(p)>=value)return;Sleep(1); }
    fprintf(stderr,"FAIL: %s: runnable producer could not run while consumer waited\n",what);exit(1);
}
static void finalize(u32 ea)
{
    for(unsigned i=0;i<3000;i++) {
        s32 rc=cellSpursFinalize((CellSpurs*)(uintptr_t)ea);
        if (!rc) return;
        assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY);Sleep(1);
    }
    assert(!"finalize failed to join after real policy return");
}
static atomic_uint held[16],done[16],gate,active_count,peak_count,lane_mask;
static void held_policy(spu_context* c)
{
    unsigned w=spu_ls_read32(c,0x1DC),lane=spu_ls_read32(c,0x1C8);
    assert(lane<3);
    atomic_fetch_or(&lane_mask,1u<<lane);
    unsigned n=atomic_fetch_add(&active_count,1)+1,peak=atomic_load(&peak_count);
    while(peak<n && !atomic_compare_exchange_weak(&peak_count,&peak,n)) {}
    atomic_fetch_add(&held[w],1);
    while(!atomic_load(&gate)) Sleep(1);
    atomic_fetch_sub(&active_count,1);atomic_fetch_add(&done[w],1);finish(c);
}
static atomic_uint fast_visits,late_seen;
static void fast_policy(spu_context* c)
{
    atomic_fetch_add(&fast_visits,1);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void late_policy(spu_context* c) { atomic_store(&late_seen,1);finish(c); }
static void stopped_policy(spu_context* c)
{
    vm_write8(spu_ls_read32(c,0x1C4)+0x80+spu_ls_read32(c,0x1DC),3);
    atomic_store(&late_seen,1);spu_halt(c);
}
static atomic_uint direct_enter,direct_leave,direct_exit;
static atomic_uintptr_t direct_ctx[2];
static int direct_rc[2];
static void resident_policy(spu_context* c)
{
    unsigned id=c->gpr[4]._u32[1]-1;   /* low half of the u64 argument (#309) */
    assert(id<2);atomic_store(&direct_ctx[id],(uintptr_t)c);
    spu_ls_write32(c,0x4000,0x12340000u+id);
    atomic_fetch_add(&direct_enter,1);
    while(atomic_load(&direct_enter)<2) Sleep(1);
    assert(spu_ls_read32(c,0x4000)==0x12340000u+id);
    atomic_fetch_add(&direct_leave,1);finish(c);
}
static void exit_policy(spu_context* c) { atomic_store(&direct_exit,1);finish(c); }
static void error_policy(spu_context* c)
{
    while(!atomic_load(&direct_exit)) Sleep(1);
    Sleep(10);spu_halt(c);
}
static void* direct(void* arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    direct_rc[id]=spu_run_policy_module(resident_policy,79,vm_base+0x9400,16,id+1,3,0x11000+id*0x1000,0);
    return NULL;
}
static void* error_direct(void* arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    direct_rc[id]=spu_run_policy_module(id?exit_policy:error_policy,79,vm_base+0x9400,16,id+1,3,0x11000+id*0x1000,0);
    return NULL;
}
static atomic_uint incarnation_runs,eligible_lane;
static void incarnation_policy(spu_context* c)
{
    assert(spu_ls_read32(c,0x4000)==0);
    spu_ls_write32(c,0x4000,0x9999);
    atomic_fetch_add(&incarnation_runs,1);finish(c);
}
static void eligible_policy(spu_context* c)
{
    atomic_store(&eligible_lane,spu_ls_read32(c,0x1C8)+1);finish(c);
}
static void register_pm(unsigned ea,unsigned byte,spu_lifted_entry_fn fn,int image)
{
    memset(vm_base+ea,byte,16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+ea,16),fn,image,"manufactured scheduler fixture");
}
int main(void)
{
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    register_pm(0x9000,0x11,consumer,77);register_pm(0x9100,0x22,producer,78);
    assert((u32)cellSpursInitialize((CellSpurs*)0x1000,7,0,0,0)==CELL_SPURS_CORE_ERROR_INVAL);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)0x1000,3,(u8*)0x8010,1)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,1)==0);
    wait_for(&entered,1,"consumer admission");
    for(unsigned i=0;i<8;i++) assert(cellSpursWakeUp((CellSpurs*)0x1000)==0);
    assert(!atomic_load(&unbounded_dispatches));
    unsigned char before[0xC0];memcpy(before,vm_base+0x1000,sizeof(before));
    assert((u32)cellSpursFinalize((CellSpurs*)0x1000)==CELL_SPURS_CORE_ERROR_BUSY);
    assert(!atomic_load(&canceled_queues) && !memcmp(before,vm_base+0x1000,sizeof(before)));
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9100,16,0,NULL,1,1)==0);
    wait_for(&finished,1,"cross-workload dependency");
    assert(atomic_load(&consumer_lane)<2 && atomic_load(&producer_lane)<2);
    assert(atomic_load(&consumer_lane)!=atomic_load(&producer_lane));
    unsigned port3=vm_read8(0x8010);
    finalize(0x1000);assert(atomic_load(&canceled_queues)==(1u<<3));
    assert(!spurs_port_queue(port3));
    puts("SPURS runnable producer / blocked consumer / BUSY immutability / join PASS");

    /* Reinitialize the same EA after all handles and resident contexts retire. */
    register_pm(0x9200,0x33,held_policy,79);
    assert(cellSpursInitialize((CellSpurs*)0x1000,3,0,0,0)==0);
    for(unsigned i=0;i<4;i++) {
        assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
        assert(vm_read32(0x8000)==i);
    }
    for(unsigned i=0;i<3;i++)wait_for(&held[i],1,"configured capacity");
    Sleep(20);assert(atomic_load(&peak_count)==3 && atomic_load(&lane_mask)==7);
    assert((u32)cellSpursRemoveWorkload((CellSpurs*)0x1000,0)==CELL_SPURS_CORE_ERROR_BUSY);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,0)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==0);
    assert(!atomic_load(&held[3]));
    for(unsigned i=0;i<3;i++)assert(atomic_load(&held[i])==1);
    atomic_store(&gate,1);
    for(unsigned i=0;i<4;i++)wait_for(&done[i],1,"release and next admission");
    finalize(0x1000);puts("SPURS configured capacity / maxContention / stable physical slots / reinit PASS");

    atomic_store(&gate,0);atomic_store(&held[0],0);atomic_store(&done[0],0);
    atomic_store(&peak_count,0);atomic_store(&lane_mask,0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,3,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,2)==0);
    wait_for(&held[0],2,"maxContention two");Sleep(20);
    assert(atomic_load(&held[0])==2 && atomic_load(&peak_count)==2);
    assert(vm_read8(0x1000+SPURS_WKL_CURCONT)==2);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,0)==0);
    Sleep(20);assert(atomic_load(&held[0])==2);
    atomic_store(&gate,1);wait_for(&done[0],2,"lowered limit preserves admitted work");
    finalize(0x1000);puts("SPURS one workload has two physical lanes and obeys lowered admission limit PASS");

    register_pm(0x9600,0x66,incarnation_policy,83);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    for(unsigned incarnation=0;incarnation<2;incarnation++) {
        assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9600,16,0,NULL,1,1)==0);
        wait_for(&incarnation_runs,incarnation+1,"same policy/data workload incarnation");
        for(unsigned i=0;;i++) {
            s32 rc=cellSpursRemoveWorkload((CellSpurs*)0x1000,0);
            if(!rc)break;assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY && i<3000);Sleep(1);
        }
    }
    finalize(0x1000);puts("SPURS removal retires resident LS even with unchanged policy/data PASS");

    register_pm(0x9700,0x77,eligible_policy,84);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    memset(vm_base+0x8020,0,8);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9700,16,0,(u8*)0x8020,1,1)==0);
    Sleep(20);assert(!atomic_load(&eligible_lane));
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,5)==0);
    vm_write16(0x1000+SPURS_WKL_SIGNAL1,0x8000);
    assert(cellSpursRemoveWorkload((CellSpurs*)0x1000,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9700,16,0,(u8*)0x8020,1,1)==0);
    assert(cellSpursReadyCountSwap((CellSpurs*)0x1000,0,(u32*)0x8030,0)==0);
    assert(vm_read32(0x8030)==0 && !vm_read16(0x1000+SPURS_WKL_SIGNAL1));
    assert(cellSpursReadyCountCompareAndSwap((CellSpurs*)0x1000,0,(u32*)0x8030,5,9)==0);
    assert(vm_read32(0x8030)==0 && vm_read8(0x1000+SPURS_WKL_READY1)==0);
    assert(cellSpursReadyCountCompareAndSwap((CellSpurs*)0x1000,0,(u32*)0x8030,0,3)==0);
    assert(vm_read8(0x1000+SPURS_WKL_READY1)==3);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,0)==0);

    vm_base[0x8021]=1;
    assert(cellSpursSetPriorities((CellSpurs*)0x1000,0,(u8*)0x8020)==0);
    wait_for(&eligible_lane,1,"zero-priority physical lane exclusion");
    assert(atomic_load(&eligible_lane)==2);finalize(0x1000);
    puts("SPURS explicit zero priority excludes physical lane / SetPriorities guest publication PASS");

    register_pm(0x9300,0x44,fast_policy,80);register_pm(0x9400,0x55,late_policy,81);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9300,16,0,NULL,1,1)==0);
    wait_for(&fast_visits,2,"continuously runnable workload");
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    wait_for(&late_seen,1,"round-robin admission");
    vm_write8(0x1080,3);finalize(0x1000);puts("SPURS continuously runnable workload does not starve later producer PASS");

    /* Public API retains global workload IDs, while instances own admission,
     * worker handles, guest state and attached-queue teardown independently. */
    atomic_store(&gate,0);atomic_store(&held[0],0);atomic_store(&done[0],0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
    wait_for(&held[0],1,"first instance active");
    atomic_store(&late_seen,0);register_pm(0x9500,0x66,stopped_policy,82);
    assert(cellSpursInitialize((CellSpurs*)0x3000,1,0,0,0)==0);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)0x3000,4,(u8*)0x8010,1)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x3000,(u32*)0x8000,(void*)0x9500,16,0,NULL,1,1)==0);
    assert(vm_read32(0x8000)==1);wait_for(&late_seen,1,"independent instance stop/error execution");
    finalize(0x3000);
    assert(s_workloads[0].in_use && s_workloads[0].spurs_ea==0x1000);
    assert(vm_read8(0x1020)==1);
    atomic_store(&gate,1);wait_for(&done[0],1,"surviving instance completion");finalize(0x1000);
    puts("SPURS independent instance admission / stop return / queue ownership PASS");

    pthread_t a,b;
    assert(!pthread_create(&a,NULL,direct,(void*)0));assert(!pthread_create(&b,NULL,direct,(void*)1));
    pthread_join(a,NULL);pthread_join(b,NULL);
    assert(direct_rc[0]==0 && direct_rc[1]==0 && atomic_load(&direct_leave)==2);
    assert(atomic_load(&direct_ctx[0])!=atomic_load(&direct_ctx[1]));
    spurs_policy_release_instance(0x11000);spurs_policy_release_instance(0x12000);
    assert(!pthread_create(&a,NULL,error_direct,(void*)0));assert(!pthread_create(&b,NULL,error_direct,(void*)1));
    pthread_join(a,NULL);pthread_join(b,NULL);assert(direct_rc[0]==-2 && direct_rc[1]==0);
    spurs_policy_release_instance(0x11000);spurs_policy_release_instance(0x12000);
    puts("SPURS same workload/lane across instances retains separate LS and TLS exit classification PASS");
    free(vm_base);return 0;
}
