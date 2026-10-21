/* Missing guest-CAS notification must fail the prompt-completion assertion.
 * The waiter, guest PPU atomic primitive, and coherence dispatcher are real. */
#undef pthread_cond_timedwait
#include "../runtime/ppu/ppu_memory.h"
#include "../runtime/spu/spu_coherency.h"
#include "../runtime/spu/spu_workload.h"
#include "../runtime/spu/spu_dma.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
extern int spu_taskset_wait_signal(uint32_t,uint32_t);
extern void spu_taskset_signal_task(uint32_t,uint32_t);
/* Hold the production waiter at the last instruction before the real POSIX
 * wait. This forces a producer into the predicate-check / sleep window without
 * test-only branches in production. All actual waiting still uses pthreads. */
static atomic_int pause_before_wait, at_wait, release_wait, producer_entered;
int signal_test_cond_wait(pthread_cond_t* cv,pthread_mutex_t* lock,const struct timespec* ts)
{
    if(atomic_exchange(&pause_before_wait,0)) {
        atomic_store(&at_wait,1);
        while(!atomic_load(&release_wait)) usleep(100);
    }
    return pthread_cond_timedwait(cv,lock,ts);
}
typedef struct { uint32_t taskset,task; atomic_int entered,done; } waiter;
static void* wait_main(void* arg)
{
    waiter* w=arg;
    atomic_store(&w->entered,1);
    assert(spu_taskset_wait_signal(w->taskset,w->task)==0);
    atomic_store(&w->done,1);
    return NULL;
}
static pthread_t start(waiter* w)
{
    pthread_t t; assert(!pthread_create(&t,NULL,wait_main,w));
    while(!atomic_load(&w->entered)) usleep(100);
    usleep(30000); /* permit the real waiter to park before the producer */
    assert(!atomic_load(&w->done));
    return t;
}
static void promptly(pthread_t t,waiter* w)
{
    unsigned long long deadline=ps3_ms_now()+250;
    while(!atomic_load(&w->done) && ps3_ms_now()<deadline) usleep(100);
    if(!atomic_load(&w->done)) {
        fprintf(stderr,"FAIL: real guest signal did not resume task within 250ms\n");
        abort();
    }
    assert(!pthread_join(t,NULL));
}
static void guest_or(uint32_t ea,uint32_t mask)
{
    ppu_context ctx={0};
    for(;;) { uint32_t old=ppu_lwarx(&ctx,ea); if(ppu_stwcx(&ctx,ea,old|mask)) return; }
}
static void* race_producer(void* unused)
{
    (void)unused; atomic_store(&producer_entered,1);
    guest_or(0x1040,0x80000000); return NULL;
}
static void remains_blocked(waiter* w)
{
    usleep(10000); assert(!atomic_load(&w->done));
}
static void count_write(void* opaque) { ++*(unsigned*)opaque; }
int main(void)
{
    unsetenv("LBP_WS_DRAIN"); unsetenv("LBP_SPU_SERIAL");
    vm_base=calloc(1,ppu_vm_size); assert(vm_base);
    /* Pre-arm as real taskset/SPU management already does: isolate the host
     * notification gap from first-publication races in the store fast path. */
    spu_lockline_lock(); spu_coh_reserve(NULL,0x1000); spu_lockline_unlock();
    waiter w={.taskset=0x1000,.task=0}; pthread_t t=start(&w);
    guest_or(0x1040,0x80000000);
    promptly(t,&w);
    assert(vm_read32(0x1040)==0);

    /* An old signal is consumed once; a zero write, another task bit or a
     * notification without a write must never manufacture another signal. */
    guest_or(0x1040,0x80000000);
    assert(spu_taskset_wait_signal(0x1000,0)==0);
    assert(vm_read32(0x1040)==0);
    w=(waiter){.taskset=0x1000,.task=0}; t=start(&w);
    vm_write32(0x1040,0); remains_blocked(&w);
    vm_write32(0x2040,0x80000000); remains_blocked(&w);
    guest_or(0x1040,0x40000000); remains_blocked(&w);
    spu_lockline_lock(); spu_coh_notify_write(0x1040); spu_lockline_unlock();
    remains_blocked(&w);
    spu_taskset_signal_task(0x1000,0); promptly(t,&w);
    assert(vm_read32(0x1040)==0x40000000);
    assert(spu_taskset_wait_signal(0x1000,1)==0);
    assert(vm_read32(0x1040)==0);

    /* Two parked consumers share a word. Each consumes only its own bit. */
    waiter a={.taskset=0x1000,.task=0},b={.taskset=0x1000,.task=1};
    pthread_t ta=start(&a),tb=start(&b);
    guest_or(0x1040,0xC0000000); promptly(ta,&a); promptly(tb,&b);
    assert(vm_read32(0x1040)==0);
    waiter high={.taskset=0x1000,.task=127}; t=start(&high);
    guest_or(0x104C,1); promptly(t,&high); assert(vm_read32(0x104C)==0);

    /* A notifier reaches the CV while the waiter owns its mutex but has
     * already released lock-line. cond_wait must release/sleep atomically. */
    atomic_store(&pause_before_wait,1);
    w=(waiter){.taskset=0x1000,.task=0}; t=start(&w);
    assert(atomic_load(&at_wait));
    pthread_t producer; assert(!pthread_create(&producer,NULL,race_producer,NULL));
    while(!atomic_load(&producer_entered)) usleep(100);
    usleep(20000);
    assert(!atomic_load(&w.done));
    atomic_store(&release_wait,1);
    promptly(t,&w); assert(!pthread_join(producer,NULL));
    assert(vm_read32(0x1040)==0);

    /* The MFC notify variant must wake host observers even if no live SPU
     * reservation exists, while leaving the self-reservation exclusion alone. */
    w=(waiter){.taskset=0x11000,.task=0}; t=start(&w);
    spu_context* dma=calloc(1,sizeof(*dma)); assert(dma);
    dma->ls=dma->ls_store;
    dma->ls[0x100]=0x80;
    assert(mfc_do_transfer(dma,0x100,0x11040,4,MFC_PUT_CMD)==0);
    promptly(t,&w); free(dma);
    assert(vm_read32(0x11040)==0);

    /* Scoped watch removal must unlink both middle/head entries; subsequent
     * coherent writes must not call a retired stack object's callback. */
    unsigned c1=0,c2=0; spu_coh_watch one,two;
    spu_lockline_lock();
    spu_coh_watch_add(&one,0x3040,count_write,&c1);
    spu_coh_watch_add(&two,0x307C,count_write,&c2);
    spu_lockline_unlock();
    vm_write32(0x3000,1); assert(c1==1 && c2==1);
    vm_write32(0x3100,1); assert(c1==1 && c2==1);
    spu_lockline_lock(); spu_coh_watch_remove(&one); spu_lockline_unlock();
    vm_write32(0x3040,2); assert(c1==1 && c2==2);
    spu_lockline_lock(); spu_coh_watch_remove(&two); spu_lockline_unlock();
    vm_write32(0x3040,3); assert(c1==1 && c2==2);
    /* Repeated real wait lifetimes exercise registration reuse and teardown. */
    for(int i=0;i<16;i++) {
        w=(waiter){.taskset=0x1000,.task=0}; t=start(&w);
        guest_or(0x1040,0x80000000); promptly(t,&w);
    }
    free(vm_base); puts("guest task signal wake, predicate, races and teardown PASS");
}
