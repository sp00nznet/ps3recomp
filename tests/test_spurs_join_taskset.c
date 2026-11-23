/* cellSpursJoinTaskset waits for the taskset's tasks to exit. Synthetic guest
 * memory only: the ENABLED bitset is set and cleared by hand, as the runtime's
 * task-exit path clears a task's bit. */
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
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls,uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)ls;(void)a;(void)b;(void)c;(void)d;return 0;}
void spu_thread_publish_ctx(uint32_t tid,void* ctx){(void)tid;(void)ctx;}

#define TS 0x4000u
static atomic_int s_cleared;
static void* exit_tasks_later(void* arg)
{
    (void)arg;
    Sleep(50);
    vm_base[TS + CSTS_ENABLED + 0] = 0;            /* task 0 exits */
    Sleep(20);
    atomic_store(&s_cleared, 1);
    vm_base[TS + CSTS_ENABLED + 15] = 0;           /* task 127 exits: the last one */
    return NULL;
}

int main(void)
{
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);

    assert((u32)cellSpursJoinTaskset(NULL)==CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert((u32)cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)(ppu_vm_size-0x40))==CELL_SPURS_TASK_ERROR_FAULT);
    puts("JoinTaskset rejects a null or out-of-range taskset PASS");

    /* No task enabled: returns at once. */
    assert(cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)TS)==CELL_OK);
    puts("JoinTaskset with no enabled task returns at once PASS");

    /* Tasks 0 and 127 enabled: Join returns only after both have exited. */
    vm_base[TS + CSTS_ENABLED + 0] = 0x80;
    vm_base[TS + CSTS_ENABLED + 15] = 0x01;
    pthread_t th; assert(pthread_create(&th,NULL,exit_tasks_later,NULL)==0);
    const ULONGLONG t0=GetTickCount64();
    assert(cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)TS)==CELL_OK);
    assert(atomic_load(&s_cleared));
    assert(GetTickCount64()-t0>=60);
    pthread_join(th,NULL);
    puts("JoinTaskset waits until every task has exited PASS");

    /* A task that never exits: bounded, BUSY. */
    s_taskset_join_timeout_ms=30;
    vm_base[TS + CSTS_ENABLED + 7] = 0x10;
    assert((u32)cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)TS)==CELL_SPURS_TASK_ERROR_BUSY);
    assert(vm_base[TS + CSTS_ENABLED + 7]==0x10);
    puts("JoinTaskset on a task that never exits returns BUSY after its bound PASS");
    return 0;
}
